.PHONE:clean

sound: audio_ring_buffer.c
	gcc -o $@ $< -lasound
clean:
	rm -rf sound
