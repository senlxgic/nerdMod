test: test_container test_fps test_audio test_audiofmt test_bmpscale test_format
	./test_container
	./test_fps
	./test_audio
	./test_audiofmt
	./test_bmpscale
	./test_format

test_format: test_format.cpp ../../../universal/include/common/nmformat.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_format.cpp

test_bmpscale: test_bmpscale.cpp ../../arm9/source/bmpscale.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_bmpscale.cpp

test_audiofmt: test_audiofmt.cpp ../../arm9/source/audiofmt.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_audiofmt.cpp -lm

test_audio: test_audio.cpp ../../arm9/source/audioring.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_audio.cpp

test_fps: test_fps.cpp ../../arm9/source/fpsutil.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_fps.cpp

test_container: test_container.cpp ../../arm9/source/videoContainer.cpp ../../arm9/source/videoContainer.h nds.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -I. -o $@ test_container.cpp ../../arm9/source/videoContainer.cpp

clean:
	rm -f test_container test_fps test_audio test_audiofmt test_bmpscale test_format
