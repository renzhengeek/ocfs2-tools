#
# SLES 12
#

TOOLSARCH = $(shell $(TOPDIR)/vendor/sles12/rpmarch.guess tools $(TOPDIR))
VENDOR_EXTENSION = SLE12
SYSTEMD_ENABLED = 1

include $(TOPDIR)/vendor/common/Vendor.make

packages: rpm
