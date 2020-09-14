
HEADERS = RaspiCamControl.hpp RaspiCLI.hpp RaspiCommonSettings.hpp RaspiHelpers.hpp gstpicam.hpp raspicam.hpp
SOURCES = RaspiCamControl.cpp RaspiCLI.cpp RaspiCommonSettings.cpp RaspiHelpers.cpp gstpicam.cpp raspicam.cpp

INCLUDES = -I /opt/vc/include/
LIBS = -L /usr/lib/arm-linux-gnueabihf/ -L /opt/vc/lib/ -lbcm_host -lmmal -lmmal_core -lmmal_util -lvcos -pthread

gstpicam.so: $(SOURCES) $(HEADERS)
	clang++ -std=c++17 -Wall $(INCLUDES) $(LIBS) -shared -o gstpicam.so $(SOURCES) $$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0)

cam_controller: cam_controller.cpp
	clang++ -std=c++17 -Wall $(INCLUDES) $(LIBS) -o cam_controller cam_controller.cpp $$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0)

.PHONY: run

run: cam_controller gstpicam.so
	./cam_controller --gst-plugin-path=$$PWD

