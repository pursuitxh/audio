#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <alsa/asoundlib.h>

//#define __USE_GNU
#include <sched.h>


#define DEBUG //also can open this when compile the file

#ifdef DEBUG
	#define PCM_THREAD_DEBUG(fmt,args...) \
		printf(fmt, ##args)	
#else
	#define	PCM_THREAD_DEBUG(fmt,args...)	
#endif

#define	PRE_READ_NODE_CNT	10
#define	FRAME_BUF_SIZE		8192

#define SERVER_PORT 		3240
#define NUM_MAX_CLIENTS 	10


typedef enum pcm_queue_ops { PCM_QUEUE_ERROR, PCM_QUEUE_OK, PCM_QUEUE_NO_NODE } pcm_queue_ops;
typedef enum pcm_queue_status_t { PCM_QUEUE_EMPTY, PCM_QUEUE_NO_EMPTY } pcm_queue_status_t;


unsigned int malloc_cnt = 0;
unsigned int write_cnt = 0;
unsigned int free_cnt = 0;


typedef struct pcm_node
{
	char*			buf_ptr;
	struct	pcm_node	*next;
} pcm_node_t, *pcm_node_ptr;

typedef struct pcm_queue
{
	pthread_mutex_t		lock;
	pcm_node_ptr		head;
	pcm_node_ptr		tail;	
} pcm_queue_t, *pcm_queue_ptr;

typedef	struct globl_control
{
	pcm_queue_t		queue;
	pthread_mutex_t		mutex;
	pthread_cond_t		cond;
	int			sock_fd;
	snd_pcm_uframes_t 	period_size;
	snd_pcm_t 		*handle;
	int			w_sleep;

} globl_control_t, *globl_control_ptr;

pcm_node_ptr pcm_alloc_buf(unsigned int size)
{
	pcm_node_ptr	node_ptr;
	node_ptr = (pcm_node_ptr)malloc(sizeof(pcm_node_t));
	node_ptr->buf_ptr = malloc(size);
	node_ptr->next = (pcm_node_ptr)0;
	malloc_cnt++;

	
	//printf("%d  malloc node ptr is  %x "
	//	"malloc node buf ptr is %x\n",malloc_cnt,node_ptr,node_ptr->buf_ptr);
	
	return node_ptr;
}

void pcm_delloc_buf(pcm_node_ptr node_ptr)
{
	free_cnt++;
	//printf("%d  free node ptr is  %x "
	//	"free node buf ptr is %x\n",free_cnt,node_ptr,node_ptr->buf_ptr);
	free(node_ptr->buf_ptr);
	free(node_ptr);
	
}

void pcm_queue_init(pcm_queue_ptr queue_ptr)
{
	pthread_mutex_init(&queue_ptr->lock,NULL);
	queue_ptr->head = queue_ptr->tail = ((pcm_node_ptr)0);
	return;
}


pcm_queue_status_t pcm_queue_get_status(pcm_queue_ptr queue_ptr)
{
	if(queue_ptr->head == NULL && queue_ptr->tail == NULL)
	{
		return PCM_QUEUE_EMPTY;
	}
	else
	{
		return PCM_QUEUE_NO_EMPTY;
	}

}

pcm_queue_ops pcm_queue_add_node(pcm_queue_ptr queue_ptr, pcm_node_ptr node_ptr)
{
	if(queue_ptr == NULL || node_ptr == NULL)
	{
		PCM_THREAD_DEBUG("pcm_queue pointer or pcm_node pointer error!");
		return PCM_QUEUE_ERROR;
	}
	pthread_mutex_lock(&queue_ptr->lock);
	if(pcm_queue_get_status(queue_ptr) == PCM_QUEUE_EMPTY)
	{
		//PCM_THREAD_DEBUG("queue is empty!!!!\n");
		queue_ptr->head = node_ptr;
		queue_ptr->tail = node_ptr;
		node_ptr->next = NULL;
	}
	else
	{
		//PCM_THREAD_DEBUG("queue is not empty!!!!\n");
		node_ptr->next = queue_ptr->tail->next;
		queue_ptr->tail->next = node_ptr;
		queue_ptr->tail = node_ptr;
	}
	pthread_mutex_unlock(&queue_ptr->lock);
	return PCM_QUEUE_OK;		
}



pcm_queue_ops pcm_queue_get_node(pcm_queue_ptr queue_ptr, pcm_node_ptr *ptr)
{
	pcm_node_ptr	node_ptr;
	pthread_mutex_lock(&queue_ptr->lock);
	if(pcm_queue_get_status(queue_ptr) == PCM_QUEUE_EMPTY)
	{
		pthread_mutex_unlock(&queue_ptr->lock);
		return PCM_QUEUE_NO_NODE;
	}
	node_ptr = queue_ptr->head;
	queue_ptr->head = node_ptr->next;
	if(queue_ptr->head == NULL)
	{
		queue_ptr->tail = NULL;
	}	

	pthread_mutex_unlock(&queue_ptr->lock);
	*ptr = node_ptr;
	return PCM_QUEUE_OK;	
}


void pcm_read_one_period_frames(globl_control_ptr gControlPtr, char *buf)
{
	int have_read = 0;
	int rd_cnt = 0;
	int sock = gControlPtr->sock_fd; 
	int frame_size = gControlPtr->period_size*4;
	while (have_read < frame_size) {
		rd_cnt = read(sock, buf + have_read, frame_size - have_read);
		have_read += rd_cnt;
	}
	return;
}

void pcm_thread_read_core(globl_control_ptr gControlPtr)
{
	unsigned int		period_size = gControlPtr->period_size;
	pcm_node_ptr		node_ptr;
	pcm_queue_ptr		queue_ptr = &gControlPtr->queue;
	int			i;	
	
	node_ptr = pcm_alloc_buf(period_size*4);
	pcm_read_one_period_frames(gControlPtr, node_ptr->buf_ptr);
	pcm_queue_add_node(queue_ptr,node_ptr);
	if(gControlPtr->w_sleep == 1)
	{
		gControlPtr->w_sleep = 0;
		i = 3;
		while(i--)
		{	
			printf("go on prefect packet!!!\n");
			node_ptr = pcm_alloc_buf(period_size*4);
			pcm_read_one_period_frames(gControlPtr, node_ptr->buf_ptr);
			pcm_queue_add_node(queue_ptr,node_ptr);

		}
		snd_pcm_prepare(gControlPtr->handle);
		pthread_cond_signal(&gControlPtr->cond);
	}
	
}

void pcm_read_thread_preact(globl_control_ptr gControlPtr)
{
	int			cnt = PRE_READ_NODE_CNT;
	
	while(cnt--)
	{	
		pcm_thread_read_core(gControlPtr);
	}
	
	pthread_cond_signal(&gControlPtr->cond);

}

void* pcm_read_thread(void* arg)
{
	globl_control_ptr	gControlPtr = (globl_control_ptr)arg;
	
	cpu_set_t		mask;
	CPU_ZERO(&mask);
	CPU_SET(0,&mask);
	sched_getaffinity(0, sizeof(mask), &mask);
		
	pcm_read_thread_preact(gControlPtr);
	
	PCM_THREAD_DEBUG("read thread be wake!!\n");
	while (1) 
	{ 
		pcm_thread_read_core(gControlPtr);
	}
	return NULL;
}


void  volatile pcm_mem_copy_wait(void)
{
	char buf_src[4096];
	char buf_des[4096];
	int  cnt = 4096;
	char* src_ptr = buf_src;
	char* des_ptr = buf_des;
	while(cnt--)
	{
		*des_ptr++ = *src_ptr++;
	}
	return;
}

void* pcm_write_thread(void* arg)
{
	globl_control_ptr	gControlPtr = (globl_control_ptr)arg;
	pcm_queue_ptr		queue_ptr = &gControlPtr->queue;
	pcm_node_ptr		node_ptr;
	pcm_queue_ops		ops;
	snd_pcm_uframes_t	period_size = gControlPtr->period_size;


	cpu_set_t		mask;
	CPU_ZERO(&mask);
	CPU_SET(1,&mask);
	sched_getaffinity(0, sizeof(mask), &mask);
	pthread_mutex_lock(&gControlPtr->mutex);
	pthread_cond_wait(&gControlPtr->cond,&gControlPtr->mutex);
	PCM_THREAD_DEBUG("wake read thread!!!!\n");
	while(1)
	{
		ops = pcm_queue_get_node(queue_ptr,&node_ptr);
		if(ops != PCM_QUEUE_NO_NODE)
		{
			PCM_THREAD_DEBUG("in write thread loop no empty!!!!\n");
			write_cnt++;
			//printf("%d  write node ptr is %x "
			//	"write node buf ptr is %x\n",write_cnt,node_ptr,node_ptr->buf_ptr);
			snd_pcm_writei(gControlPtr->handle, node_ptr->buf_ptr, period_size);
			//pcm_mem_copy_wait();
			pcm_delloc_buf(node_ptr);
			//usleep(100);
		}
		else
		{
			pthread_mutex_init(&gControlPtr->mutex,NULL);
			pthread_cond_init(&gControlPtr->cond,NULL);
			gControlPtr->w_sleep = 1;
			PCM_THREAD_DEBUG("write thread wait condition\n");
			pthread_mutex_lock(&gControlPtr->mutex);
			pthread_cond_wait(&gControlPtr->cond,&gControlPtr->mutex);
			PCM_THREAD_DEBUG("write thread condition happen\n");
			PCM_THREAD_DEBUG("%d in write thread loop empty!!!!\n", write_cnt);

		}
	}
}

void pcm_sock_init(int* fd)
{
	int ret;
	int sin_size;
	int client_socket;
	int port_reuse = 1;
	int sock_fd;

	struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(SERVER_PORT);
	server_addr.sin_addr.s_addr = INADDR_ANY;
	bzero(&(server_addr.sin_zero), 8);

   	 /*create a socket*/
	if((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		PCM_THREAD_DEBUG("can not get a socket!\n");
		exit(1);
	}

	/*set to reuse the addr&port*/
	//if((setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &port_reuse, sizeof(int))) == -1) {
	//	PCM_THREAD_DEBUG("addr port reuse error!\n");
	//    	exit(1);
	//}

	/* set no delay attribute */
	const int val = 1;
	ret = setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
	if (ret < 0) {
		PCM_THREAD_DEBUG("setsockopt TCP_NODELAY");
		exit(1);
	}

	/*bind*/
	if(bind(sock_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
		PCM_THREAD_DEBUG("bind error\n");
		exit(1);
	}

	//TODO: should use main thread to listen another clients to connect
   	/*listen*/
	if(listen(sock_fd, NUM_MAX_CLIENTS) == -1) {
		PCM_THREAD_DEBUG("listen error\n");
		exit(1);
	}
	printf("listening...\n");

	/*accept*/
	sin_size = sizeof(struct sockaddr_in);
    	if((*fd = accept(sock_fd, (struct sockaddr *)&client_addr, &sin_size)) == -1) {
		PCM_THREAD_DEBUG("accept error\n");
		exit(1);
	}

	printf("accept success!\n");

	return ;

}

void pcm_globl_control_init(globl_control_ptr gControlPtr)
{
	pcm_queue_init(&gControlPtr->queue);
	pthread_mutex_init(&gControlPtr->mutex,NULL);
	pthread_cond_init(&gControlPtr->cond,NULL);
	pcm_sock_init(&gControlPtr->sock_fd);
	gControlPtr->w_sleep = 0;
	return;
}

void audio_device_init(globl_control_ptr gControlPtr)
{
	
	int ret;
	int dir = 0;
	unsigned int val;
	snd_pcm_hw_params_t *pParams;
	snd_pcm_uframes_t buffer_size;

	/* Open PCM device for playback. */
	ret = snd_pcm_open(&gControlPtr->handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
	if (ret < 0) { 
		fprintf(stderr, "unable to open pcm device: %s\n", snd_strerror(ret));
		exit(1);
	}

	/* Allocate a hardware parameters object. */
	snd_pcm_hw_params_alloca(&pParams);

	/* Fill it in with default values. */
	snd_pcm_hw_params_any(gControlPtr->handle, pParams);

	/* Set the desired hardware parameters. */

	/* Interleaved mode */
	snd_pcm_hw_params_set_access(gControlPtr->handle, pParams, SND_PCM_ACCESS_RW_INTERLEAVED);

	/* Signed 16-bit little-endian format */
	snd_pcm_hw_params_set_format(gControlPtr->handle, pParams, SND_PCM_FORMAT_S16_LE);

	/* Two channels (stereo) */
	snd_pcm_hw_params_set_channels(gControlPtr->handle, pParams, 2);

	/* 44100 bits/second sampling rate (CD quality) */
	val = 44100;
	snd_pcm_hw_params_set_rate_near(gControlPtr->handle, pParams, &val, &dir);

	/* Set period size to 2048 frames. */
	gControlPtr->period_size = 2048;
	ret = snd_pcm_hw_params_set_period_size_near(gControlPtr->handle, pParams, &gControlPtr->period_size, &dir);
	printf("period size is : %d frames\n", gControlPtr->period_size);
	/* Write the parameters to the driver */
	ret = snd_pcm_hw_params(gControlPtr->handle, pParams);
	if (ret < 0){ 
		fprintf(stderr, "unable to set hw parameters: %s\n", snd_strerror(ret));
		exit(1);
	}
	
	/* Use a buffer large enough to hold one period */
	snd_pcm_hw_params_get_period_size(pParams, &gControlPtr->period_size, &dir);
	snd_pcm_hw_params_get_buffer_size(pParams, &buffer_size);

#ifdef DEBUG
	printf("period size is : %d frames\n", gControlPtr->period_size);
	printf("buffer size is : %d frames\n", buffer_size);
#endif
}

int main( int argc, char** argv )
{
	globl_control_t		gControl;
	pthread_t		r_tid;
	pthread_t		w_tid;
	int			ret;


	audio_device_init(&gControl);
	pcm_globl_control_init(&gControl);
	ret = pthread_create(&r_tid,NULL,pcm_read_thread,&gControl);
	if(ret != 0)
	{
		PCM_THREAD_DEBUG("read thread create error!\n");
		return -1;
	}
	ret = pthread_create(&w_tid,NULL,pcm_write_thread,&gControl);
	if(ret != 0)
	{
		PCM_THREAD_DEBUG("write thread create error\n");
		return -1;
	}
	pthread_join(r_tid,NULL);
	pthread_join(w_tid,NULL);
	return 0;	
}
