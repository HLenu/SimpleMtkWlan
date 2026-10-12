// mtkwifi: command line Wi-Fi control for SimpleMtkWlan.kext (works in the macOS installer's Terminal).
//
//   mtkwifi                      status
//   mtkwifi scan                 list nearby networks
//   mtkwifi join SSID [PASSWORD] connect (no password = open network), then request DHCP
//   mtkwifi disconnect
//   mtkwifi power on|off
//   mtkwifi phy [a|n|ac|ax]      set the highest mode, reconnect without reboot
//
// It speaks the same ClientKit ioctls HeliPort uses.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ETHER_ADDR_LEN 6
#include "../include/ClientKit/Common.h"

static io_connect_t open_driver(void)
{
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("SimpleMtkWlan"));
	if (!svc) {
		fprintf(stderr,
			"No registered MediaTek Wi-Fi service.\n"
			"The kext may be loaded but failed to probe or start the card.\n"
			"Run macOS-extras/wifi-load.sh to check loading and collect the startup report.\n");
		exit(1);
	}
	io_connect_t con = 0;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &con);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "cannot open the Wi-Fi driver (0x%x)\n", kr);
		exit(1);
	}
	return con;
}

static kern_return_t ioctl_get(io_connect_t con, int id, void *data, size_t len) {
	return IOConnectCallStructMethod(con, id, NULL, 0, data, &len);
}

static kern_return_t ioctl_set(io_connect_t con, int id, void *data, size_t len) {
	return IOConnectCallStructMethod(con, id | IOCTL_MASK, data, len, NULL, 0);
}

static const char *state_name(int s) {
	static const char *names[] = {"idle", "scanning", "authenticating", "associating", "connected"};
	return (s >= 0 && s <= 4) ? names[s] : "unknown";
}

static void fmt_ssid(const unsigned char *raw, char out[NWID_LEN + 1]) {
	memcpy(out, raw, NWID_LEN);
	out[NWID_LEN] = 0;
}

static const char *security(const struct ioctl_network_info *n) {
	if (n->supported_rsnprotos == 0)
		return "open";
	if (n->supported_rsnakms & MTK80211_AKM_8021X)
		return "enterprise";
	if (n->supported_rsnprotos & MTK80211_PROTO_RSN)
		return "WPA2";
	return "WPA";
}

static int bsd_name(io_connect_t con, char out[32]) {
	struct ioctl_driver_info info = {.version = IOCTL_VERSION};
	if (ioctl_get(con, IOCTL_80211_DRIVER_INFO, &info, sizeof(info)) != KERN_SUCCESS)
		return 0;
	memcpy(out, info.bsd_name, 32);
	out[31] = 0;
	return out[0] != 0;
}

static int cmd_status(io_connect_t con)
{
	struct ioctl_driver_info info = {.version = IOCTL_VERSION};
	struct ioctl_power power = {.version = IOCTL_VERSION};
	struct ioctl_state state = {.version = IOCTL_VERSION};
	kern_return_t result = ioctl_get(con, IOCTL_80211_DRIVER_INFO, &info, sizeof(info));
	if (result != KERN_SUCCESS) {
		fprintf(stderr, "Wi-Fi service opened, but driver status failed (0x%x)\n", result);
		return 1;
	}
	printf("Interface : %.32s\nDriver    : %.32s\nFirmware  : %.32s\n", info.bsd_name, info.driver_version, info.fw_version);
	if (ioctl_get(con, IOCTL_80211_POWER, &power, sizeof(power)) == KERN_SUCCESS)
		printf("Power     : %s\n", power.enabled ? "on" : "off");
	if (ioctl_get(con, IOCTL_80211_STATE, &state, sizeof(state)) == KERN_SUCCESS)
		printf("State     : %s\n", state_name(state.state));
	if (state.state == MTK80211_S_RUN) {
		struct ioctl_sta_info sta = {.version = IOCTL_VERSION};
		if (ioctl_get(con, IOCTL_80211_STA_INFO, &sta, sizeof(sta)) == KERN_SUCCESS) {
			char ssid[NWID_LEN + 1];
			static const char *modes[] = {"802.11a", "802.11b", "802.11g", "802.11n", "802.11ac", "802.11ax"};
			fmt_ssid(sta.ssid, ssid);
			printf("Network   : %s (%02x:%02x:%02x:%02x:%02x:%02x)\n", ssid, sta.bssid[0], sta.bssid[1],
				sta.bssid[2], sta.bssid[3], sta.bssid[4], sta.bssid[5]);
			printf("Mode      : %s\n", (unsigned)sta.op_mode < 6 ? modes[sta.op_mode] : "unknown");
			printf("Channel   : %u (%u MHz wide)\nSignal    : %d dBm (noise %d)\nRX rate   : %u Mbit/s\n",
				sta.channel, sta.band_width, sta.rssi, sta.noise, sta.rate);
		}
	}
	return 0;
}

static int cmd_scan(io_connect_t con) {
	struct ioctl_power power = {.version = IOCTL_VERSION, .enabled = 1};
	ioctl_set(con, IOCTL_80211_POWER, &power, sizeof(power));
	struct ioctl_scan scan = {.version = IOCTL_VERSION};
	if (ioctl_set(con, IOCTL_80211_SCAN, &scan, sizeof(scan)) != KERN_SUCCESS)
		fprintf(stderr, "scan request failed (is Wi-Fi powered on? try: mtkwifi power on)\n");
	printf("Scanning...\n");
	sleep(5);
	printf("%-32s %6s %4s  %s\n", "SSID", "dBm", "CH", "SECURITY");
	struct ioctl_network_info n;
	int count = 0;
	while (count < 200 && ioctl_get(con, IOCTL_80211_SCAN_RESULT, &n, sizeof(n)) == KERN_SUCCESS) {
		char ssid[NWID_LEN + 1];
		fmt_ssid(n.ssid, ssid);
		printf("%-32s %6d %4u  %s\n", ssid[0] ? ssid : "(hidden)", n.rssi, n.channel, security(&n));
		count++;
	}
	if (count == 0)
		printf("(no networks found yet; run the scan again)\n");
	return 0;
}

static int cmd_join(io_connect_t con, const char *ssid, const char *pwd) {
	size_t slen = strlen(ssid), plen = strlen(pwd);
	// The driver treats both fields as C strings, so they need room for the terminator.
	if (slen == 0 || slen >= NWID_LEN) {
		fprintf(stderr, "SSID must be 1-31 characters\n");
		return 2;
	}
	if (plen >= WPA_KEY_LEN || (plen > 0 && plen < 8)) {
		fprintf(stderr, "WPA password must be 8-63 characters (or empty for an open network)\n");
		return 2;
	}

	struct ioctl_power power = {.version = IOCTL_VERSION, .enabled = 1};
	ioctl_set(con, IOCTL_80211_POWER, &power, sizeof(power));

	struct ioctl_associate as;
	memset(&as, 0, sizeof(as));
	as.version = IOCTL_VERSION;
	as.nwid.version = IOCTL_VERSION;
	as.nwid.len = (unsigned)slen;
	memcpy(as.nwid.nwid, ssid, slen);
	as.wpa_key.version = IOCTL_VERSION;
	as.wpa_key.len = (unsigned)plen;
	memcpy(as.wpa_key.key, pwd, plen);
	if (ioctl_set(con, IOCTL_80211_ASSOCIATE, &as, sizeof(as)) != KERN_SUCCESS) {
		fprintf(stderr, "the driver rejected the connection request\n");
		return 1;
	}

	printf("Connecting to \"%s\"", ssid);
	fflush(stdout);
	struct ioctl_state state = {.version = IOCTL_VERSION};
	int last = -1;
	for (int i = 0; i < 40; i++) {
		sleep(1);
		if (ioctl_get(con, IOCTL_80211_STATE, &state, sizeof(state)) != KERN_SUCCESS)
			continue;
		if (state.state != last) {
			printf("\n  %s", state_name(state.state));
			last = state.state;
		} else {
			printf(".");
		}
		fflush(stdout);
		if (state.state == MTK80211_S_RUN)
			break;
	}
	printf("\n");
	if (state.state != MTK80211_S_RUN) {
		fprintf(stderr, "not connected after 40 s (wrong password, out of range, or the driver needs a look at its log)\n"
			"Save the driver log now, before rebooting: bash /Volumes/MACOS_USB/macOS-extras/check.sh\n");
		return 1;
	}

	char ifname[32];
	if (bsd_name(con, ifname)) {
		char cmd[96];
		// configd normally starts DHCP by itself; asking again is harmless and helps in the installer.
		snprintf(cmd, sizeof(cmd), "ipconfig set %s DHCP >/dev/null 2>&1", ifname);
		system(cmd);
		printf("Connected. Waiting for an IP address on %s...\n", ifname);
		snprintf(cmd, sizeof(cmd), "ipconfig getifaddr %s", ifname);
		for (int i = 0; i < 15; i++) {
			FILE *p = popen(cmd, "r");
			char ip[64] = {0};
			if (p) {
				if (!fgets(ip, sizeof(ip), p))
					ip[0] = 0;
				pclose(p);
			}
			if (ip[0]) {
				printf("IP address: %s", ip);
				return 0;
			}
			sleep(1);
		}
		printf("No IP address yet. If it stays that way, run: sudo ipconfig set %s DHCP\n", ifname);
	} else {
		printf("Connected.\n");
	}
	return 0;
}

static int cmd_disconnect(io_connect_t con) {
	struct ioctl_sta_info sta = {.version = IOCTL_VERSION};
	ioctl_get(con, IOCTL_80211_STA_INFO, &sta, sizeof(sta));
	struct ioctl_disassociate dis;
	memset(&dis, 0, sizeof(dis));
	dis.version = IOCTL_VERSION;
	memcpy(dis.ssid, sta.ssid, NWID_LEN);
	return ioctl_set(con, IOCTL_80211_DISASSOCIATE, &dis, sizeof(dis)) == KERN_SUCCESS ? 0 : 1;
}

static int cmd_phy(io_connect_t con, const char *mode)
{
	static const char *names[] = {"a", "n", "ac", "ax"};
	struct ioctl_phy_limit phy = {.version = IOCTL_VERSION};
	kern_return_t result;

	if (mode) {
		for (phy.limit = 0; phy.limit < 4; phy.limit++) {
			if (!strcmp(mode, names[phy.limit]))
				break;
		}
		if (phy.limit == 4) {
			fprintf(stderr, "PHY mode must be a, n, ac or ax\n");
			return 2;
		}
		result = ioctl_set(con, IOCTL_80211_PHY_LIMIT, &phy, sizeof(phy));
	} else {
		result = ioctl_get(con, IOCTL_80211_PHY_LIMIT, &phy, sizeof(phy));
	}
	if (result != KERN_SUCCESS) {
		fprintf(stderr, "PHY control failed (0x%x); requires driver 1.0.20 or newer\n", result);
		return 1;
	}
	if (phy.limit > 3)
		return 1;
	printf("PHY limit: %s%s\n", names[phy.limit], mode ? " (reconnecting if Wi-Fi was active)" : "");
	return 0;
}

static void usage(void) {
	fprintf(stderr,
		"usage: mtkwifi                       status\n"
		"       mtkwifi scan                  list networks\n"
		"       mtkwifi join SSID [PASSWORD]  connect (quote names with spaces)\n"
		"       mtkwifi disconnect\n"
		"       mtkwifi power on|off\n"
		"       mtkwifi phy [a|n|ac|ax]       view or change the highest Wi-Fi mode\n");
	exit(2);
}

int main(int argc, char **argv) {
	io_connect_t con = open_driver();
	int rc = 0;
	if (argc == 1 || !strcmp(argv[1], "status"))
		rc = cmd_status(con);
	else if (!strcmp(argv[1], "scan"))
		rc = cmd_scan(con);
	else if (!strcmp(argv[1], "join") && (argc == 3 || argc == 4))
		rc = cmd_join(con, argv[2], argc == 4 ? argv[3] : "");
	else if (!strcmp(argv[1], "disconnect"))
		rc = cmd_disconnect(con);
	else if (!strcmp(argv[1], "phy") && (argc == 2 || argc == 3))
		rc = cmd_phy(con, argc == 3 ? argv[2] : NULL);
	else if (!strcmp(argv[1], "power") && argc == 3 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
		struct ioctl_power power = {.version = IOCTL_VERSION, .enabled = !strcmp(argv[2], "on")};
		rc = ioctl_set(con, IOCTL_80211_POWER, &power, sizeof(power)) == KERN_SUCCESS ? 0 : 1;
	} else
		usage();
	IOServiceClose(con);
	return rc;
}
