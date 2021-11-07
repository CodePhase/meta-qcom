SUMMARY = "Small utility to sync current date from network"
LICENSE = "MIT"
MY_PN = "timesync"
RPROVIDES_${PN} = "timesync"
PR = "r7"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://src/timesync.h file://src/timesync.c"

S = "${WORKDIR}"

do_compile() {
    ${CC} ${LDFLAGS} -O2 src/timesync.c -o timesync
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/timesync ${D}${bindir}
}

pkg_postinst:${PN}() {
   #!/bin/sh
   echo "TS:12345:boot:/usr/bin/timesync" >> $D/etc/inittab
}