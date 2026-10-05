test: test_container test_fps test_audio test_audiofmt test_bmpscale test_format test_font test_weather test_net test_photopolicy test_audioclass test_reclog test_sdbench test_resample test_container_v2
	./test_container
	./test_fps
	./test_audio
	./test_audiofmt
	./test_bmpscale
	./test_format
	./test_font
	./test_weather
	./test_net
	./test_photopolicy
	./test_audioclass
	./test_reclog
	./test_sdbench
	./test_resample
	./test_container_v2

test_reclog: test_reclog.cpp ../../arm9/source/reclog.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_reclog.cpp

test_audioclass: test_audioclass.cpp ../../arm9/source/audiofmt.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_audioclass.cpp

test_photopolicy: test_photopolicy.cpp ../../../universal/include/common/nmphoto.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_photopolicy.cpp

test_net: test_net.cpp ../../../universal/include/common/nmnet.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_net.cpp

test_weather: test_weather.cpp ../../../universal/include/common/nmweather.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_weather.cpp

test_font: test_font.cpp ../../../universal/include/common/nmfont.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_font.cpp

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
	rm -f test_container_v2 test_reclog test_audioclass test_container test_fps test_audio test_audiofmt test_bmpscale test_format test_font test_weather test_net test_font test_photopolicy

test_sdbench: test_sdbench.cpp ../../arm9/source/sdbenchcalc.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_sdbench.cpp

test_resample: test_resample.cpp ../../arm9/source/audioresample.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -o $@ test_resample.cpp -lm

test_container_v2: test_container_v2.cpp ../../arm9/source/videoContainer.cpp ../../arm9/source/videoContainer.h ../../arm9/source/videofmt.h nds.h
	g++ -std=gnu++17 -O1 -g -Wall -Wextra -I. -o $@ test_container_v2.cpp ../../arm9/source/videoContainer.cpp
