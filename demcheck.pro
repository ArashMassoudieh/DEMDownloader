QT += core network
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = demcheck

INCLUDEPATH += src

SOURCES += main.cpp \
    TnmClient.cpp \
    CsvTable.cpp \
    SiteProcessor.cpp

HEADERS += \
    Types.h \
    ProductType.h \
    TnmClient.h \
    CsvTable.h \
    SiteProcessor.h
