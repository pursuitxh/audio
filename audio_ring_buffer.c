/*
 * Receive pcm data from internet and play it on local
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/tcp.h> 
#include <pthread.h>
#include <semaphore.h>
#include <alsa/asoundlib.h>

#define DEBUG

#ifdef DEBUG

#define dbg(format, arg...) \
	do {					\
		printf("SOUNDDEBUG: %s(%s) " format, __FILE__, __func__, ##arg);\
	} while(0)

#define dbg_alsa(format, arg...) \
	do {                         \
		fprintf(stderr, format, ##arg); \
	} while(0)

#define dbg_socket(a) perror(a)

#else

#define dbg(a, arg...)
#define dbg_alsa(a, arg...)
#define dbg_socket(a)

#endif

#define SERVER_PORT 3250
#define NUM_MAX_CLIENTS 10

#define QUEUE_SIZE	20

typedef struct msg {
	int type;
	char pcm_msg[8192];
}msg_t;

msg_t g_msg;
int g_count = 0;

typedef enum queue_status {
	FULL = 0,
	EMPTY,
	HAVE_DATA
}queue_status_t;

typedef struct queue {
	pthread_mutex_t lock;
	sem_t wait;
	sem_t full;
	unsigned int read_pos;
	unsigned int write_pos;
	msg_t msg[QUEUE_SIZE];
	queue_status_t status;
}queue_t;

typedef struct common_data {
	queue_t queue;
	snd_pcm_t *handle;
	snd_pcm_uframes_t period_size;
	size_t chunk_bytes;
	int server_socket;
	int client_socket;
}common_data_t;

static int queue_init(common_data_t *p_common_data)
{
	pthread_mutex_init(&p_common_data->queue.lock, NULL);
	sem_init(&p_common_data->queue.wait, 0, 0);
	sem_init(&p_common_data->queue.full, 0, 20);

	p_common_data->queue.read_pos = 0;
	p_common_data->queue.write_pos = 0;

	return 0;
}

static int queue_length(queue_t queue)
{
	return (queue.write_pos-queue.read_pos+QUEUE_SIZE)%QUEUE_SIZE;
}

static queue_status_t add_item_to_queue(queue_t *p_queue, msg_t msg)
{
	pthread_mutex_lock(&p_queue->lock);

	if((p_queue->write_pos+1)%QUEUE_SIZE == p_queue->read_pos){
		p_queue->status = FULL;
		pthread_mutex_unlock(&p_queue->lock);
		return p_queue->status;
	}

	p_queue->msg[p_queue->write_pos] = msg;
	p_queue->write_pos = (p_queue->write_pos+1)%QUEUE_SIZE;

	pthread_mutex_unlock(&p_queue->lock);

	return HAVE_DATA;
}

static queue_status_t read_item_from_queue(queue_t *p_queue, msg_t *msg)
{
	pthread_mutex_lock(&p_queue->lock);

	/* check if queue is empty */
	if(p_queue->read_pos == p_queue->write_pos) {
		p_queue->status == EMPTY;
		pthread_mutex_unlock(&p_queue->lock);
		return p_queue->status;
	}

	/* read a data from read position */
	*msg = p_queue->msg[p_queue->read_pos];
	p_queue->read_pos = (p_queue->read_pos+1)%QUEUE_SIZE;

	pthread_mutex_unlock(&p_queue->lock);

	return HAVE_DATA;
}

static int init_audio_device(common_data_t *p_common_data)
{
	int ret;
	int dir = 0;
	unsigned int val;
	snd_pcm_hw_params_t *p_params;
	snd_pcm_uframes_t buffer_size;
	unsigned int buffer_time;
	unsigned int period_time;
	snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
	size_t bits_per_sample;
	size_t bits_per_frame;

	ret = snd_pcm_open(&p_common_data->handle, "plughw:0,0", SND_PCM_STREAM_PLAYBACK, 0);
	if (ret < 0) { 
		dbg_alsa("unable to open pcm device: %s\n", snd_strerror(ret));
		return -1;
	}

	snd_pcm_hw_params_alloca(&p_params);

	snd_pcm_hw_params_any(p_common_data->handle, p_params);

	snd_pcm_hw_params_set_access(p_common_data->handle, p_params, SND_PCM_ACCESS_RW_INTERLEAVED);

	snd_pcm_hw_params_set_format(p_common_data->handle, p_params, SND_PCM_FORMAT_S16_LE);

	snd_pcm_hw_params_set_channels(p_common_data->handle, p_params, 2);

	val = 44100;
	snd_pcm_hw_params_set_rate_near(p_common_data->handle, p_params, &val, &dir);




	snd_pcm_hw_params_get_buffer_time_max(p_params, &buffer_time, 0);

//	if (buffer_time > 500000)
//		buffer_time = 500000;
//
//	period_time = buffer_time / 4;
//
//	snd_pcm_hw_params_set_period_time_near(p_common_data->handle, p_params, &period_time, 0);
//	snd_pcm_hw_params_set_buffer_time_near(p_common_data->handle, p_params, &buffer_time, 0);

	p_common_data->period_size = 2048;
	snd_pcm_hw_params_set_period_size_near(p_common_data->handle, p_params, &p_common_data->period_size, &dir);

	ret = snd_pcm_hw_params(p_common_data->handle, p_params);
	if (ret < 0){ 
		dbg_alsa("unable to set hw parameters: %s\n", snd_strerror(ret));
		exit(1);
	}
	
	ret = snd_pcm_state(p_common_data->handle);
	dbg("state is %d\n", ret);
	snd_pcm_hw_params_get_period_size(p_params, &p_common_data->period_size, &dir);
	snd_pcm_hw_params_get_buffer_size(p_params, &buffer_size);

	if (p_common_data->period_size == buffer_size) {
		dbg_alsa("Can't use period size equal to buffer size (%lu == %lu)\n", p_common_data->period_size, buffer_size);
	}

	dbg("period size is : %lu frames\n", p_common_data->period_size);
	dbg("buffer size is : %lu frames\n", buffer_size);

	bits_per_sample = snd_pcm_format_physical_width(format);
	bits_per_frame = bits_per_sample * 2;
	p_common_data->chunk_bytes = p_common_data->period_size * bits_per_frame / 8;

	//g_buf = (msg_t *)malloc(sizeof(msg_t)+(char)p_common_data->chunk_bytes);

	dbg("sample rate is %d\n", val);
	dbg("bits_per_sample %d\n", bits_per_sample);
	dbg("bits_per_frame %d\n", bits_per_frame);
	dbg("chunk_bytes %d\n", p_common_data->chunk_bytes);
	dbg("PCM handle name = '%s'\n",snd_pcm_name(p_common_data->handle));

	return 0;
}

int setup_socket(common_data_t *p_common_data)
{
	int ret;
	int sin_size;
	int port_reuse = 1;
	const int val = 1;

	struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(SERVER_PORT);
	server_addr.sin_addr.s_addr = INADDR_ANY;
	bzero(&(server_addr.sin_zero), 8);

	if((p_common_data->server_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		dbg_socket("can not get a socket!\n");
		return -1;
	}
	if((setsockopt(p_common_data->server_socket, SOL_SOCKET, SO_REUSEADDR, &port_reuse, sizeof(int))) == -1) {
		dbg_socket("addr port reuse error!\n");
		return -1;
	}
	ret = setsockopt(p_common_data->server_socket, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
	if (ret < 0) {
		dbg_socket("setsockopt tcp nodelay error\n");
		return -1;
	}
	if(bind(p_common_data->server_socket, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
		dbg_socket("bind error\n");
		return -1;
	}
	//TODO: should use main thread to listen another clients to connect
	if(listen(p_common_data->server_socket, NUM_MAX_CLIENTS) == -1) {
		dbg_socket("listen error\n");
		exit(1);
	}
	printf("listening...\n");
	sin_size = sizeof(struct sockaddr_in);
    if((p_common_data->client_socket = accept(p_common_data->server_socket, (struct sockaddr *)&client_addr, &sin_size)) == -1) {
		dbg_socket("accept error\n");
	}
	printf("accept success!\n");

	return 0;
}

void read_one_period_frames(common_data_t *p_common_data)
{
	int ret;
	int have_read = 0;

	while (have_read < p_common_data->chunk_bytes) {
		if((ret = recv(p_common_data->client_socket, g_msg.pcm_msg + have_read, p_common_data->chunk_bytes - have_read, MSG_WAITALL)) == -1) {
		   perror("receive error...\n");
		   close(p_common_data->client_socket);
		   pthread_exit(NULL);
		}
		if(0 == ret) {
		   perror("client socket is closed!\n");
		   close(p_common_data->client_socket);
		   pthread_exit(NULL);
		}

		have_read += ret;
//		dbg("have_read %d\n", have_read);
	}
//	if(g_count < 1)
//		g_count++;
//	else
//		sem_post(&p_common_data->queue.wait);
}

void *recv_pcmdata(void *arg)
{

	dbg("Enter recv_pcmdata thread....\n");

	int ret;
	fd_set recvfds;

	int length;

	common_data_t *p_common_data = (common_data_t *)arg;

	struct timeval timeout;	
	timeout.tv_sec = 2;
	timeout.tv_usec = 0;

	while (1) { 
	timeout.tv_sec = 2;
	timeout.tv_usec = 0;

        FD_ZERO(&recvfds);
        FD_SET(p_common_data->client_socket, &recvfds);
		switch(select(p_common_data->client_socket + 1, &recvfds, NULL, NULL, &timeout)) {
			case -1:	             
				dbg("select error!\n");
				close(p_common_data->client_socket);
				pthread_exit(NULL);
			case 0:
				usleep(1000);
				break;//Time out
			default:
				if(FD_ISSET(p_common_data->client_socket, &recvfds) > 0) {
					read_one_period_frames(p_common_data);
					sem_wait(&p_common_data->queue.full);
					if((add_item_to_queue(&p_common_data->queue, g_msg)) == FULL){
						dbg("queue is full...\n");
						//TODO:sleep for a while?
						continue;
					}
					length = queue_length(p_common_data->queue);
					dbg("queue length is %d\n", length);
					//TODO:
					sem_post(&p_common_data->queue.wait);
				}
		}
	}
}

void *write_pcmdata(void *arg)
{
	dbg("Enter write_pcmdata thread.....\n");

	int ret;
	msg_t pcm_data;
	snd_pcm_sframes_t avail;

	common_data_t *p_common_data = (common_data_t *)arg;
	dbg("PCM handle name = '%s'\n",snd_pcm_name(p_common_data->handle));
	dbg("period szie = %lu\n",p_common_data->period_size);

	while (1) {
		sem_wait(&p_common_data->queue.wait);
		if((read_item_from_queue(&p_common_data->queue, &pcm_data)) == EMPTY){
			dbg("queue is empty...\n");
			//TODO:block?
			continue;
		}
		snd_pcm_prepare(p_common_data->handle);
		ret = snd_pcm_writei(p_common_data->handle, pcm_data.pcm_msg, p_common_data->period_size);
		if (ret == -EAGAIN) { //means try again
			/* wait 1000ms for pcm device to become ready */
			dbg_alsa("EAGAIN error, Sleep for 1000ms\n");
			snd_pcm_wait(p_common_data->handle, 1000);
		}
		else if (ret == -EPIPE) {
			/* EPIPE means underrun */
			snd_pcm_prepare(p_common_data->handle);
			dbg_alsa("underrun occurred\n");
		} 
		else if(ret == -ESTRPIPE) {
			dbg_alsa("Need suspend\n");
		}
		else if (ret < 0) {
			dbg_alsa("error from writei: %s\n", snd_strerror(ret));
		}  
		ret = snd_pcm_state(p_common_data->handle);
		dbg("state is %d\n", ret);
		sem_post(&p_common_data->queue.full);
	}
}

int main()
{
	int ret;
	common_data_t common_data;

	pthread_t recv_tid;
	pthread_t write_tid;

	queue_init(&common_data);

	ret = init_audio_device(&common_data);
	if(ret == -1){
		dbg("init audio device error.\n");
		exit(1);
	}

	ret = setup_socket(&common_data);
	if(ret == -1){
		dbg("setup socket error.\n");
		return 0;
	}

	/* create two threads: socket recv thread and pcm write thread */
	ret = pthread_create(&recv_tid, NULL, recv_pcmdata, (void *)&common_data);
	if (0 != ret){
		dbg("can't create recv_pcmdata thread\n");
	}

	ret = pthread_create(&write_tid, NULL, write_pcmdata, (void *)&common_data);
	if (0 != ret){
		dbg("can't create write_thread\n");
	}

	pthread_join(recv_tid, NULL);
//	pthread_join(write_tid, NULL);

	close(common_data.client_socket);
	close(common_data.server_socket);
	//free(g_buf);
	snd_pcm_drain(common_data.handle);
	snd_pcm_close(common_data.handle);

	return 0;
}
