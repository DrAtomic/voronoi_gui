IMGUI_DIR = ./deps/imgui

INCLUDE_PATHS = -I.
INCLUDE_PATHS += -I/usr/include/SDL3
INCLUDE_PATHS += -I$(IMGUI_DIR)
INCLUDE_PATHS += -I$(IMGUI_DIR)/backends

APP = voronoi

PLUG = lib_plug.so

SHADERS = voronoi.vert.spv voronoi.frag.spv

CXXFLAGS = -Wall -Wextra -g
LIBS = -lGL -ldl `pkg-config sdl3 --libs`
LINK_OPTS = -Wl,-rpath=/usr/local/lib/

all: $(SHADERS) $(APP) $(PLUG)

voronoi.vert.spv: voronoi.vert
	glslangValidator -V -S vert $< -o $@

voronoi.frag.spv: voronoi.frag
	glslangValidator -V -S frag $< -o $@

imgui.o: imgui.cpp
	g++ -c $(CXXFLAGS) $(INCLUDE_PATHS) imgui.cpp -o imgui.o

$(APP): main.cpp imgui.o backend_abstraction.cpp $(SHADERS)
	g++ $(CXXFLAGS) -rdynamic $(INCLUDE_PATHS) main.cpp -o $(APP) imgui.o $(LIBS) $(LINK_OPTS)

plug.o: plug.cpp plug.h
	g++ -c $(CXXFLAGS) -fPIC $(INCLUDE_PATHS) plug.cpp -o plug.o

$(PLUG): plug.o
	g++ -shared -o $(PLUG) plug.o -ldl

clean:
	rm -rf $(APP) $(PLUG) *.o *.so *.spv compile_commands.json .cache imgui.ini
