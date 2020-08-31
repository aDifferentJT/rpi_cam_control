
HEADERS = RaspiCamControl.hpp RaspiCLI.hpp RaspiCommonSettings.hpp RaspiHelpers.hpp
SOURCES = cam_controller.cpp RaspiCamControl.cpp RaspiCLI.cpp RaspiCommonSettings.cpp RaspiHelpers.cpp

INCLUDES = -I /opt/vc/include/
LIBS = -L /usr/lib/arm-linux-gnueabihf/ -L /opt/vc/lib/ -lbcm_host -lmmal -lmmal_core -lmmal_util -lvcos -lccrtp -lucommon -lcommoncpp -pthread

cam_controller: $(SOURCES) $(HEADERS)
	clang++ -std=c++17 -Wall $(INCLUDES) $(LIBS) -o cam_controller $(SOURCES)
