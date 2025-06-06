QT += core gui widgets

CONFIG += c++11

TARGET = VoxelEditor
TEMPLATE = app

SOURCES += main.cpp


HEADERS +=

# Include paths for Irrlicht
INCLUDEPATH += /Users/macbook2015/Downloads/irrlicht-1.8.5/include /Users/macbook2015/Desktop/brew/include/2

# Library paths for Irrlicht
LIBS += -L/Users/macbook2015/Downloads/irrlicht-1.8.5/lib/mac -L/Users/macbook2015/Desktop/brew/lib -lpng -lIrrlicht -framework Foundation -framework OpenGL -framework Cocoa -framework Carbon -framework AppKit -framework IOKit

# Add any other necessary Qt or system libraries here
