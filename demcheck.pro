QT += core network
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = demcheck

INCLUDEPATH += src

SOURCES += main.cpp \
    TigerClient.cpp \
    MrlcClient.cpp \
    TnmClient.cpp \
    CsvTable.cpp \
    SiteProcessor.cpp \
    ShapefileWriter.cpp

HEADERS += \
    TigerClient.h \
    MrlcClient.h \
    Types.h \
    ProductType.h \
    TnmClient.h \
    CsvTable.h \
    SiteProcessor.h \
    ShapefileWriter.h
