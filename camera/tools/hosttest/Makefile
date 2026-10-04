test: test_container test_fps
	./test_container
	./test_fps

test_fps: test_fps.cpp ../../arm9/source/fpsutil.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_fps.cpp

test_container: test_container.cpp ../../arm9/source/videoContainer.cpp ../../arm9/source/videoContainer.h nds.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -I. -o $@ test_container.cpp ../../arm9/source/videoContainer.cpp

clean:
	rm -f test_container test_fps
