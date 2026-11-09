// SPDX-License-Identifier: GPL-2.0-only
/* Предпросмотр экрана на хосте: рисует кадр с условными данными в RGB565.
 * Режимы: 0 = всё хорошо, 1 = предупреждения, 2 = авария, 3 = светлая тема,
 * 8 = страница модема без SIM, 9 = страница модема с LTE, 10 = главная с модемом без SIM,
 * 11 = главная без podkop и модема. Запуск: preview frame.rgb565 <режим> */
#define PREVIEW
#include "almond-lcd-status.c"
#include "m2-sample.h"

int main(int argc, char **argv)
{
	static struct state st;
	int i, mode = argc > 2 ? atoi(argv[2]) : 0, builtin = 0, bare = 0;
	FILE *f;

	if (mode == 11) {	/* без podkop и без модема */
		bare = 1;
		mode = 0;
	}
	if (mode >= 8 && mode <= 10) {
		builtin = mode - 7;	/* 1 = GSM без SIM, 2 = LTE с SIM, 3 = главная страница */
		mode = 0;
	}

	st.bat_ok = 1;
	st.bat_adc = mode == 1 ? 640 : mode == 2 ? 560 : 690;
	st.bat_stat = mode == 1 || mode == 2 ? 0x00 : mode == 3 ? BAT_EXT : BAT_EXT | BAT_IDLE;
	if (mode == 3) { theme_idx = 1; mode = 0; }
	st.s = (struct slow){ .ts = 1, .fakeip = 1, .ping_ms = 47, .podkop_table = 1, .vpn_ok_age = 30,
			  .has_podkop = !bare, .has_modem = !bare };
	snprintf(st.s.guard, sizeof(st.s.guard), mode == 1 ? "warn" : "ok");
	snprintf(st.s.vpn_ip, sizeof(st.s.vpn_ip), "203.0.113.10");
	st.slow_fresh = 1;
	st.s.modem_ping = 1;
	st.s.lte_ok = mode != 2;
	snprintf(st.s.lte_mode, sizeof(st.s.lte_mode), "LTE");
	st.s.lte_rsrp = mode == 1 ? -103 : -54;
	st.s.lte_rsrq = -10.5;
	st.s.lte_sinr = mode == 1 ? 4 : 15;
	st.s.lte_temp = 44;
	snprintf(st.s.lte_bands, sizeof(st.s.lte_bands), "B7+B1+B3+B8");
	st.s.lte_age = 40;
	st.s.lte_sim = 1;
	for (i = 0; i < HIST; i++) {
		st.rx_hist[i] = 1.5e6 * (1 + sin(i / 9.0)) + (i % 17 == 0 ? 4e6 : 0);
		st.tx_hist[i] = 0.3e6 * (1 + cos(i / 13.0));
	}
	st.rx = st.rx_hist[HIST - 1];
	st.tx = st.tx_hist[HIST - 1];
	st.cpu_pct = mode == 2 ? 97 : 23;
	st.ncpu = 4;
	st.core_pct[0] = mode == 2 ? 100 : 41; st.core_pct[1] = mode == 2 ? 95 : 12;
	st.core_pct[2] = mode == 2 ? 99 : 30; st.core_pct[3] = mode == 2 ? 92 : 8;
	st.mem_tot = 249068;
	st.mem_av = 36000;
	snprintf(st.loadavg, sizeof(st.loadavg), "0.64 0.48 0.41");
	st.uptime = 12 * 86400 + 19 * 3600 + 10 * 60;
	st.sb_alive = mode != 2;
	st.tun_cfg = 0;
	st.tun_alive = st.tun_cfg && mode != 1;
	st.wf[0] = (struct wifi){ .label = "2.4GHz", .up = 1, .clients = 13, .rx_act = 1, .tx_act = 0 };
	st.wf[1] = (struct wifi){ .label = "5GHz", .up = mode != 2, .clients = 2, .rx_act = 1, .tx_act = 1 };
	st.wanw = (struct wifi){ .label = "WAN", .up = 1, .rx_act = 1, .tx_act = mode != 1 };
	st.rx_idle = mode == 2 ? 14 : 0.2;
	if (mode == 2) st.s.modem_ping = 1;
	m2_init(&st.m2);
	if (builtin) {
		const char *smp = builtin == 2 ? m2_sample_builtin_lte : m2_sample_builtin_gsm;

		st.s.lte_sim = builtin == 2;
		snprintf(st.s.lte_mode, sizeof(st.s.lte_mode), builtin == 2 ? "LTE" : "GSM");
		st.s.lte_rsrp = builtin == 2 ? -92 : -83;
		st.s.lte_sinr = builtin == 2 ? 9.4 : 0;
		st.s.lte_temp = builtin == 2 ? 44 : 42;
		snprintf(st.s.lte_bands, sizeof(st.s.lte_bands), builtin == 2 ? "B7" : "");
		st.s.lte_age = 0;
		st.page = builtin != 3;
		m2_apply(&st.m2, smp, time(NULL) - 60);
		m2_apply(&st.m2, smp, time(NULL));
		for (i = 0; i < M2_HIST; i++) {	/* час истории: RSSI для GSM, SINR для LTE */
			st.m2.rsrp_hist[i] = i < 12 ? NAN : -83 + 4 * sin(i / 7.0) - (i > 40 && i < 44 ? 12 : 0);
			st.m2.sinr_hist[i] = builtin == 2 ? (i < 12 ? NAN : 9 + 3 * sin(i / 6.0)) : NAN;
		}
		st.m2.mtime = time(NULL) - 5;
	}
	st.phase = theme_idx;		/* в светлой теме мигающее деление зарядки горит */
	render_page(&st);
	f = fopen(argc > 1 ? argv[1] : "frame.rgb565", "wb");
	fwrite(fb, sizeof(fb), 1, f);
	fclose(f);
	return 0;
}
