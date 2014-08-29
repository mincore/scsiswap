#
# Makefile for drivers/scsi/arm
#
obj-$(CONFIG_SCSI_SWAP_BADSECTORS) += scsi_swap.o
scsi_swap-y += swap.o core.o log.o sysfs.o utils.o crc32.o

scsi_swap-$(CONFIG_SCSI_SIM_BADSECTORS) += sim.o
