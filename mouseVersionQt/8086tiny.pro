QT += core gui widgets
CONFIG += c++17
TARGET = 8086tiny_qt
TEMPLATE = app

SOURCES += 8086tiny.cpp

# Keep TMT for text-mode rendering (needed for DOS text output)
# Remove USE_TMT if you only care about graphics-mode programs like allycat.exe
DEFINES += QT_PORT

# NO_AUDIO strips the SDL audio callback (allycat.exe doesn't need PC speaker)
DEFINES += NO_AUDIO

# Point to your tmt library if using USE_TMT
# INCLUDEPATH += /path/to/libtmt
# LIBS += -ltmt

HEADERS +=

LIBS += -L/Users/macbook2015/Downloads/libdogecoin-msvc-0.1.1-dev/.libs -L/Users/macbook2015/Desktop/brew/lib

INCLUDEPATH += /Users/macbook2015/Downloads/libdogecoin-msvc-0.1.1-dev/include /Users/macbook2015/Desktop/brew/include /Users/macbook2015/Desktop/brew/lib /Users/macbook2015/Downloads/opencv-4.x/build

