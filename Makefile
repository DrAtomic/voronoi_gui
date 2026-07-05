IMGUI_DIR = ./deps/imgui

INCLUDE_PATHS = -I.
INCLUDE_PATHS += -I$(IMGUI_DIR)
INCLUDE_PATHS += -I$(IMGUI_DIR)/backends

CXXFLAGS = -Wall -Wextra -g

all: voronoi libplug.so

voronoi: main.cpp imgui.o backends.o backend_abstraction.cpp plug_reload.cpp voronoi.cpp
	g++ $(CXXFLAGS) $(INCLUDE_PATHS) -rdynamic main.cpp -o voronoi imgui.o backends.o -lGL `pkg-config --static --libs glfw3`

libplug.so: plug.cpp plug.h
	g++ $(CXXFLAGS) $(INCLUDE_PATHS) -fPIC -shared plug.cpp -o libplug.so

backends.o: backends.cpp
	g++ $(CXXFLAGS) $(INCLUDE_PATHS) -c backends.cpp -o backends.o

imgui.o: imgui.cpp backends.o
	g++ $(CXXFLAGS) $(INCLUDE_PATHS) -c imgui.cpp -o imgui.o

clean:
	rm -rf voronoi libplug.so *.o *.so compile_commands.json .cache imgui.ini
