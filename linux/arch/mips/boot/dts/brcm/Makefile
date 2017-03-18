dtb-$(CONFIG_DT_BCM93384WVG)		+= bcm93384wvg.dtb
dtb-$(CONFIG_DT_BCM93384WVG_VIPER)	+= bcm93384wvg_viper.dtb
dtb-$(CONFIG_DT_BCM96368MVWG)		+= bcm96368mvwg.dtb
dtb-$(CONFIG_DT_BCM9EJTAGPRB)		+= bcm9ejtagprb.dtb
dtb-$(CONFIG_DT_BCM97125CBMB)		+= bcm97125cbmb.dtb
dtb-$(CONFIG_DT_BCM97346DBSMB)		+= bcm97346dbsmb.dtb
dtb-$(CONFIG_DT_BCM97358SVMB)		+= bcm97358svmb.dtb
dtb-$(CONFIG_DT_BCM97360SVMB)		+= bcm97360svmb.dtb
dtb-$(CONFIG_DT_BCM97362SVMB)		+= bcm97362svmb.dtb
dtb-$(CONFIG_DT_BCM97420C)		+= bcm97420c.dtb
dtb-$(CONFIG_DT_BCM97241CBSV)		+= bcm97241cbsv.dtb
dtb-$(CONFIG_DT_BCM97241DCSFBTSFF)	+= bcm97241dcsfbtsff.dtb
dtb-$(CONFIG_DT_BCM97241USFF)		+= bcm97241usff.dtb
dtb-$(CONFIG_DT_BCM97425C)		+= bcm97425c.dtb
dtb-$(CONFIG_DT_BCM97425SVMB)		+= bcm97425svmb.dtb
dtb-$(CONFIG_DT_BCM97425VMSMB)		+= bcm97425vmsmb.dtb
dtb-$(CONFIG_DT_BCM97425VMSMB_SFF)	+= bcm97425vmsmb_sff.dtb
dtb-$(CONFIG_DT_BCM97428C)		+= bcm97428c.dtb
dtb-$(CONFIG_DT_BCM97428GN)		+= bcm97428gn.dtb
dtb-$(CONFIG_DT_BCM97428SFF)		+= bcm97428sff.dtb
dtb-$(CONFIG_DT_BCM97428SV)		+= bcm97428sv.dtb
dtb-$(CONFIG_DT_BCM97429DVR)		+= bcm97429dvr.dtb
dtb-$(CONFIG_DT_BCM97429SVMB)		+= bcm97429svmb.dtb
dtb-$(CONFIG_DT_BCM97435C)		+= bcm97435c.dtb
dtb-$(CONFIG_DT_BCM97435SVMB)		+= bcm97435svmb.dtb
dtb-$(CONFIG_DT_BCM97435VMSMB_SFF)	+= bcm97435vmsmb_sff.dtb
dtb-$(CONFIG_DT_BCM974XX)		+= bcm97241cbsv.dtb \
					   bcm97241dcsfbtsff.dtb \
					   bcm97241usff.dtb \
					   bcm97425c.dtb \
					   bcm97425svmb.dtb \
					   bcm97425vmsmb.dtb \
					   bcm97425vmsmb_sff.dtb \
					   bcm97428c.dtb \
					   bcm97428gn.dtb \
					   bcm97428sff.dtb \
					   bcm97428sv.dtb \
					   bcm97429dvr.dtb \
					   bcm97429svmb.dtb \
					   bcm97435c.dtb \
					   bcm97435svmb.dtb \
					   bcm97435vmsmb_sff.dtb

obj-y				+= $(patsubst %.dtb, %.dtb.o, $(dtb-y))

# Force kbuild to make empty built-in.o if necessary
obj-				+= dummy.o

always				:= $(dtb-y)
clean-files			:= *.dtb *.dtb.S
