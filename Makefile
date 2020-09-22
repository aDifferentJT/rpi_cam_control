
INCLUDES = -I /opt/vc/include/
LIBS = -L /usr/lib/arm-linux-gnueabihf/ -L /opt/vc/lib/ -lbcm_host -lmmal -lmmal_core -lmmal_util -lvcos -pthread

cam_controller: cam_controller.cpp
	clang++ -std=c++17 -Wall $(INCLUDES) $(LIBS) -o cam_controller2 cam_controller2.cpp $$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-app-1.0 gstreamer-plugins-bad-1.0 gstreamer-video-1.0)

.PHONY: run

run: cam_controller
	./cam_controller

