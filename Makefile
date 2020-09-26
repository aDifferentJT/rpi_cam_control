
INCLUDES = -I /opt/vc/include/
LIBS = -L /usr/lib/arm-linux-gnueabihf/ -L /opt/vc/lib/ -lbcm_host -lmmal -lmmal_core -lmmal_util -lvcos -luSockets -lz -pthread

cam_controller: cam_controller.cpp
	clang++ -std=c++17 -Wall $(INCLUDES) -o cam_controller cam_controller.cpp $(LIBS) $$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-app-1.0 gstreamer-plugins-bad-1.0 gstreamer-video-1.0)

.PHONY: run

run: cam_controller
	./cam_controller

