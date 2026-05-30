IMGUI_DIR = ./deps/imgui

INCLUDE_PATHS = -I.
INCLUDE_PATHS += -I$(IMGUI_DIR)
INCLUDE_PATHS += -I$(IMGUI_DIR)/backends

APP = voronoi

PLUG = lib_plug.so

CXXFLAGS = -Wall -Wextra -g
LIBS = -lGL -ldl `pkg-config --static --libs glfw3`
LINK_OPTS = -Wl,-rpath=/usr/local/lib/

all: $(APP) $(PLUG)

backends.o: backends.cpp
	g++ -c $(CXXFLAGS) $(INCLUDE_PATHS) backends.cpp -o backends.o

imgui.o: imgui.cpp backends.o
	g++ -c $(CXXFLAGS) $(INCLUDE_PATHS) imgui.cpp -o imgui.o

$(APP): main.cpp imgui.o backend_abstraction.cpp voronoi.cpp
	g++ $(CXXFLAGS) -rdynamic $(INCLUDE_PATHS) main.cpp -o $(APP) imgui.o backends.o $(LIBS) $(LINK_OPTS)

plug.o: plug.cpp plug.h
	g++ -c $(CXXFLAGS) -fPIC $(INCLUDE_PATHS) plug.cpp -o plug.o

$(PLUG): plug.o
	g++ -shared -o $(PLUG) plug.o -ldl

clean:
	rm -rf $(APP) $(PLUG) *.o *.so *.spv compile_commands.json .cache imgui.ini
