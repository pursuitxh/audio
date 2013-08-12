.PHONE:clean

sound: sound_ring_buffer.c
	gcc -o $@ $< -lasound
clean:
	rm -rf sound
