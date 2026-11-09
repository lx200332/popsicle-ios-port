/*
 * PANDORA SENSORS -- evidence, not a driver. There is nothing on this board to drive yet.
 *
 * pandora_sensors=1 prints what the two benchmark trees actually say about the sensor path,
 * and stops. It deliberately touches no hardware: on this SoC the sensors are not on the AP's
 * I2C buses at all, and the one thing that could be probed blindly (an address scan of the 26
 * GENI I2C controllers) would be guessing at a bus whose slaves the DT does not describe.
 *
 * What the evidence says (sources quoted so the next person does not have to re-derive it):
 *
 * 1. Device tree: canoe_base.dts has 26 i2c@ nodes (0xa80000, 0xa84000, 0xa88000, 0xa8c000,
 *    0xa90000, 0xa94000, 0xa98000, 0x880000, 0x884000, 0x888000, 0x88c000, 0x890000, 0x1980000,
 *    0x1984000, 0x1988000, 0x198c000, 0x1990000, 0x1a80000, 0x1a84000, 0x1a88000, 0x1a8c000,
 *    0x980000, 0x984000, 0x988000, 0x98c000, 0x990000; all reg = <base 0x4000>). Exactly one of
 *    them carries a child device: i2c@a8c000 -> wcd939x_i2c@e, the audio codec. No accelerometer,
 *    gyroscope, magnetometer, ambient light, proximity or barometer node exists anywhere in the
 *    tree (grep for the vendor prefixes returns zero in both dts/canoe_base.dts and
 *    dts/popsicle_overlay.dts).
 *
 * 2. The Android image ships this hardware as ADSP work, not AP work: out/vendor/etc/sensors/
 *    hals.conf lists sensors.qsh.so, sensors.dynamic_sensor_hal.so, sensors.touch.detect.so,
 *    vl53l8.hal@2.0.so and sensors.ultrasoundproximity.so; out/vendor/bin has sscrpcd,
 *    ssc_sensor_info, qsap_sensors and the sensors multihal; out/dsp/adsp has the SSC skeletons
 *    (libsns_device_mode_skel.so, libsns_direct_channel_skel.so, libsns_dynamic_loader_skel.so,
 *    libsns_remote_proc_state_skel.so, sns_tppe.so). So the accelerometer and gyroscope are behind
 *    Snapdragon Sensor Core on the ADSP, reached over the same QMI/GPR channel pandora_adsp.c
 *    already brings up -- not over I2C. The two exceptions are themselves not I2C ICs we can read:
 *    vl53l8.hal@2.0.so is ST's VL53L8 time-of-flight part and sensors.ultrasoundproximity.so is an
 *    ultrasound proximity path.
 *
 * 3. The only trace of a part number in either tree is a historical shell command in
 *    _work/ios-port/asst.txt line 4340, reading calibration blobs out of the device's persist
 *    kit: sensors/registry/registry/sm8850_icm4x6xx_0.json.icm4x6xx_0_platform.accel.fac_cal.bias.
 *    That names an InvenSense ICM4x6xx family part and proves the device has factory accelerometer
 *    bias calibration, but it is a filename in a log, not a driver, an address, or a DT node. It is
 *    recorded here as a lead, not as a fact about which bus the part sits on.
 *
 * 4. No SSC/SLPI client exists on the XNU side. pandora_adsp.c starts the ADSP and sets up
 *    GPR/GLINK; nothing above it speaks the sns_ service protocol. So even the parts that are on
 *    the ADSP have no path into the kernel yet.
 *
 * To turn any of this into a driver, four things have to exist first, in this order:
 *   a) a decision on which side owns the parts: read the ICM over I2C from the AP (needs the bus
 *      number and address, which the DT does not give), or keep them on the ADSP and speak sns_
 *      over GPR (needs the SSC skeletons' protocol, and the ADSP image's sensor registry);
 *   b) for the I2C route: the GENI I2C master path (the register map is already in pandora_bt.c
 *      for the UART side of the same block; I2C adds the command packing at SE_GENI_M_CMD0 0x600);
 *   c) for the ADSP route: a QMI/GPR client on top of pandora_adsp.c's channel, plus the
 *      registry JSON from /mnt/vendor/persist/sensors/registry (sns_reg_config points there);
 *   d) interrupt wiring either way: the TOF part's GPIO and the SSC's doorbell are both absent
 *      from the tree, so there is no line to attach.
 *
 * Until (a) is decided, any code below this comment would be invention. That is why this file
 * prints and returns. Boot-arg pandora_sensors=0 (the default) keeps it silent.
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>

void pandora_sensors_init(void);

/* What is known per class, so the boot log is the survey table itself and does not need a
 * separate document to be read next to it. Six plain prints rather than a table walked in a
 * loop: -Wunsafe-buffer-usage (which the CI line enables) flags any subscript of an array, and a
 * table read through a subscript would be the only warning in this file. */
#define PS_ROW(k, s) printf("PANDORA SENSORS: %-14s %s\n", (k), (s))

void
pandora_sensors_init(void)
{
	int on = 0;

	(void)PE_parse_boot_argn("pandora_sensors", &on, sizeof(on));
	if (!on) {
		return;
	}
	printf("PANDORA SENSORS: not adapted -- no sensor node in this tree's DTS "
	    "(26 i2c buses, only wcd939x_i2c@e present); sensor ICs are behind the ADSP/SSC "
	    "(Sensor Core) which has no XNU driver\n");
	PS_ROW("accelerometer", "ADSP/SSC (icm4x6xx family per persist registry name); no AP i2c node");
	PS_ROW("gyroscope",     "ADSP/SSC (same part as the accelerometer); no AP i2c node");
	PS_ROW("magnetometer",  "not in this tree at all -- no node, no part number, no calibration blob");
	PS_ROW("ambient light", "not in this tree at all -- no node, no part number");
	PS_ROW("proximity",     "vl53l8.hal@2.0.so (ST VL53L8 ToF) + sensors.ultrasoundproximity.so, both ADSP");
	PS_ROW("barometer",     "not in this tree at all -- no node, no part number");
	printf("PANDORA SENSORS: needs, in order: (a) AP-i2c vs ADSP/SSC ownership decision, "
	    "(b) GENI I2C master path or a sns_ GPR client, (c) registry from "
	    "/mnt/vendor/persist/sensors/registry, (d) interrupt wiring (absent from the tree)\n");
}
#endif /* PANDORA */
