QT -= gui
QT += websockets

CONFIG += c++latest console
CONFIG -= app_bundle

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
        SignalingServer.cpp \
        main.cpp \
        core/ClientRegistry.cpp \
        core/SignalingApiHandler.cpp \
        core/SignalingRelay.cpp \
        external_api/external_api_protocol_facade.cpp \
        third/sqlite/sqlite3.c \
        db/manage_db.cpp

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

HEADERS += \
        SignalingServer.h \
        core/ClientRegistry.h \
        core/SignalingApiHandler.h \
        core/SignalingRelay.h \
        external_api/external_api_protocol_constants.h \
        external_api/external_api_models.h \
        external_api/external_api_envelope_types.h \
        external_api/external_api_forward_catalog.h \
        external_api/external_api_protocol_facade.h \
        db/manage_db.h

INCLUDEPATH += $$PWD $$PWD/third/sqlite
