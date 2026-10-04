test: test_container
	./test_container

test_container: test_container.cpp ../../arm9/source/videoContainer.cpp ../../arm9/source/videoContainer.h nds.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -I. -o $@ test_container.cpp ../../arm9/source/videoContainer.cpp

clean:
	rm -f test_container
