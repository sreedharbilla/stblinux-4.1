obj-y				+= regsave.o
obj-$(CONFIG_ARM)		+= s2-arm.o pm-arm.o
obj-$(CONFIG_ARM64)		+= s2-arm64.o pm-arm.o
obj-$(CONFIG_BMIPS_GENERIC)	+= s2-mips.o s3-mips.o pm-mips.o
obj-$(CONFIG_BRCMSTB_XPT_HASH)	+= xpt_dma.o

# Default sed regexp - multiline due to syntax constraints
define sed-y
	"/^->/{s:->#\(.*\):/* \1 */:; \
	s:^->\([^ ]*\) [\$$#]*\([-0-9]*\) \(.*\):#define \1 \2 /* \3 */:; \
	s:^->\([^ ]*\) [\$$#]*\([^ ]*\) \(.*\):#define \1 \2 /* \3 */:; \
	s:->::; p;}"
endef

# Use filechk to avoid rebuilds when a header changes, but the resulting file
# does not
define filechk_offsets
	(set -e; \
	 echo "#ifndef $2"; \
	 echo "#define $2"; \
	 echo "/*"; \
	 echo " * DO NOT MODIFY."; \
	 echo " *"; \
	 echo " * This file was generated by Kbuild"; \
	 echo " */"; \
	 echo ""; \
	 sed -ne $(sed-y); \
	 echo ""; \
	 echo "#endif" )
endef

# Need to specify full path to the objects
BRCMSTB_PM_DIR := drivers/soc/brcmstb/pm/

LINUXINCLUDE += -I$(objtree)/$(BRCMSTB_PM_DIR)

drivers/soc/brcmstb/pm/pm_data-offsets.s: drivers/soc/brcmstb/pm/pm_data-offsets.c
	$(call if_changed_dep,cc_s_c)

drivers/soc/brcmstb/pm/pm_data-offsets.h: drivers/soc/brcmstb/pm/pm_data-offsets.s FORCE
	$(call filechk,offsets,__PM_DATA_OFFSETS_H__)

drivers/soc/brcmstb/pm/s2-arm.o: drivers/soc/brcmstb/pm/pm_data-offsets.h

drivers/soc/brcmstb/pm/s2-arm64.o: drivers/soc/brcmstb/pm/pm_data-offsets.h
