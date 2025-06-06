QT += core gui widgets

CONFIG += c++17

TARGET = VoxelEditor
TEMPLATE = app

SOURCES += main.cpp \
    tmt.cpp


HEADERS += \
    tmt.h

# Include paths for Irrlicht
INCLUDEPATH += /Users/macbook2015/Downloads/irrlicht-1.8.5/include /Users/macbook2015/Desktop/brew/include/

# Library paths for Irrlicht
LIBS += -L/Users/macbook2015/Downloads/irrlicht-1.8.5/lib/mac -L/Users/macbook2015/Desktop/brew/lib -lpng -lSDL -lIrrlicht -framework Foundation -framework OpenGL -framework Cocoa -framework Carbon -framework AppKit -framework IOKit

# Add any other necessary Qt or system libraries here
