#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_flash_data_types.h"
#include "rom/crc.h"

#include <string.h>
#include <map>
#include <vector>

//#include "odroid_sdcard.h"
#include "odroid_display.h"
#include "odroid_input.h"
#include "SimpleList.h"

#include "../components/ugui/ugui.h"
#include "vendor.h"

static uint16_t *fb[4];

typedef struct {
    uint8_t   ap;
    uint8_t   ch;
    uint8_t * mac;
    uint32_t* pkts;
    uint64_t* time;
    signed int* rssi;
} station_t;

UG_GUI gui;
char tempstring[512];
char vendorstring[64];

uint64_t last_time = 0;

static void pset(UG_S16 x, UG_S16 y, UG_COLOR color)
{
	assert(x < 320);
	assert(y < 240);
    fb[y / 60][(y % 60) * 320 + x] = color;
}

static void ui_update_display()
{
    ili9341_write_frame(fb);
    /*
    ili9341_write_frame_rectangleLE(0, 0, 320, 60, fb[0]);
    ili9341_write_frame_rectangleLE(0, 60, 320, 60, fb[1]);
    ili9341_write_frame_rectangleLE(0, 120, 320, 60, fb[2]);
    ili9341_write_frame_rectangleLE(0, 180, 320, 60, fb[3]);
    */
}

static void UpdateDisplay()
{
    ui_update_display();
}

static void DisplayMessage(char const * message)
{
    UG_FontSelect(&FONT_8X12);
    short left = (320 / 2) - (strlen(message) * 9 / 2);
    short top = (240 / 2) + 8 + (12 / 2) + 16;
    UG_SetForecolor(C_BLACK);
    UG_SetBackcolor(C_WHITE);
    UG_FillFrame(0, top, 319, top + 12, C_WHITE);
    UG_PutString(left, top, message);

    UpdateDisplay();
}

//---------------
void boot_application()
{
    backlight_deinit();

    esp_restart();
}


#define ESP_PARTITION_TABLE_OFFSET CONFIG_PARTITION_TABLE_OFFSET /* Offset of partition table. Backwards-compatible name.*/
#define ESP_PARTITION_TABLE_MAX_LEN 0xC00 /* Maximum length of partition table data */
#define ESP_PARTITION_TABLE_MAX_ENTRIES (ESP_PARTITION_TABLE_MAX_LEN / sizeof(esp_partition_info_t)) /* Maximum length of partition table data, including terminating entry */

#define PART_TYPE_APP 0x00
#define PART_SUBTYPE_FACTORY 0x00

static void ui_draw_title()
{
    UG_FillFrame(0, 0, 319, 239, C_WHITE);

    // Header
    UG_FillFrame(0, 0, 319, 15, C_MIDNIGHT_BLUE);
    UG_FontSelect(&FONT_8X8);

    UG_SetForecolor(C_WHITE);
    UG_SetBackcolor(C_MIDNIGHT_BLUE);
    UG_PutString(4, 4, "ODROID-GO");

    // Footer
    UG_FillFrame(0, 239 - 16, 319, 239, C_MIDNIGHT_BLUE);
    //const short footerLeft = (320 / 2) - (strlen(VERSION) * 9 / 2);
    //UG_SetForecolor(C_DARK_GRAY);
    //UG_PutString(footerLeft, 240 - 4 - 8, VERSION);
}

volatile int scan_finished = 0;
volatile int scanning = 0;

esp_err_t event_handler(void *ctx, system_event_t *event){
	if (event->event_id == SYSTEM_EVENT_SCAN_DONE) {
		scan_finished = 2;
	}
	return ESP_OK;
}

#define ITEMS_APS 4
#define ITEMS_STAS 8

wifi_ap_record_t *ap;
SimpleList<station_t> stations;

const char *wifi_auth_types[] = {
		"Open",
		"WEP",
		"WPA_PSK",
		"WPA2_PSK",
		"WPA_WPA2_PSK",
		"WPA2_ENTERPRISE",
		"MAX"
};

const char *wifi_cipher_types[] = {
		"None",
		"WEP40",
		"WEP104",
		"TKIP",
		"CCMP",
		"TKIP_CCMP",
		"Unknown"
};

void print_sta_info(int page, int line, int textLeft, int top) {
    station_t cur_sta = stations.get(page + line);

    memset(vendorstring, 0, 64);
    searchVendor(cur_sta.mac);

    int64_t diff_time = esp_timer_get_time() - *cur_sta.time;

    UG_COLOR color;

    if (diff_time > 10000000) {
        color = (C_BLACK);
    } else if (diff_time > 5000000) {
        color = (C_RED);
    } else if (diff_time > 1000000) {
        color = (C_ORANGE);
    } else {
        color = (C_GREEN);
    }

    sprintf(tempstring, "%02X:%02X:%02X:%02X:%02X:%02X, %s, %3ddb, ch: %u\nData: %u, Mgmt: %u, Ctrl: %u", cur_sta.mac[0], cur_sta.mac[1], cur_sta.mac[2], cur_sta.mac[3], cur_sta.mac[4], cur_sta.mac[5], vendorstring, *cur_sta.rssi, cur_sta.ch, cur_sta.pkts[2], cur_sta.pkts[0], cur_sta.pkts[1]);
    UG_PutString(textLeft, top + 5, tempstring);
    UG_FillCircle(310, top + 13, 5, color);
}

void print_ap_info(int page, int line, int textLeft, int top) {
    wifi_ap_record_t *cur_ap = ap + page + line;
    char country[3] = {cur_ap->country.cc[0], cur_ap->country.cc[1], 0};
    if (country[0] < 'A' || country[0] > 'Z') {
        country[0] = '-';
        country[1] = '-';
    }

    memset(vendorstring, 0, 64);
    searchVendor(cur_ap->bssid);

    sprintf(tempstring, "SSID: %s\nCH: %2d, RSSI: %3ddb, %s, %s\nPairC: %s, GroupC: %s\n%02X:%02X:%02X:%02X:%02X:%02X, Vendor: %s", cur_ap->ssid, cur_ap->primary, cur_ap->rssi, country, wifi_auth_types[cur_ap->authmode], wifi_cipher_types[cur_ap->pairwise_cipher], wifi_cipher_types[cur_ap->group_cipher], cur_ap->bssid[0], cur_ap->bssid[1], cur_ap->bssid[2], cur_ap->bssid[3], cur_ap->bssid[4], cur_ap->bssid[5], vendorstring);	        
    UG_PutString(textLeft, top + 9, tempstring);
}

void draw_page(uint32_t num_items, uint32_t current_item, uint8_t items_per_page, void (*text_fct)(int, int, int, int)) {
	const int innerHeight = 240 - (16 * 2); // 208
	const int itemHeight = innerHeight / items_per_page; // 52

	const short textLeft = 4;

	int page = current_item / items_per_page;
	page *= items_per_page;

	UG_FillFrame(0, 15, 319, 222, C_WHITE);
    UG_FontSelect(&FONT_6X8);

	if (num_items > 0) {
		for (int line = 0; line < items_per_page; ++line) {
			if (page + line >= num_items) break;

            short top = 16 + (line * itemHeight) - 1;

            UG_FillFrame(2, top - 1, 317, top, C_GRAY);
            UG_FillFrame(2, top + itemHeight - 1, 317, top + itemHeight, C_GRAY);

	        if ((page) + line == current_item)
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_YELLOW);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 3, C_YELLOW);
	        }
	        else
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_WHITE);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 3, C_WHITE);
	        }

            (*text_fct)(page, line, textLeft, top);
		}
        sprintf(tempstring, "        %d/%d", current_item + 1, num_items);
        UG_SetForecolor(C_WHITE);
        UG_SetBackcolor(C_MIDNIGHT_BLUE);
        UG_FontSelect(&FONT_8X8);
        UG_PutString(320 - strlen(tempstring) * 9 - 10, 240 - 4 - 8, tempstring);

		ui_update_display();
	}
}

void start_scan(wifi_scan_config_t *scan_conf) {
    UG_FontSelect(&FONT_8X8);
    UG_SetForecolor(C_WHITE);
    UG_SetBackcolor(C_MIDNIGHT_BLUE);

    UG_PutString(4, 240 - 4 - 8, "scanning wifi              ");

    ui_update_display();
    scan_finished = 1;
    gpio_set_level(GPIO_NUM_2, 1);
    ESP_ERROR_CHECK(esp_wifi_scan_start(scan_conf, false));
}

typedef struct {
	unsigned frame_ctrl:16;
	unsigned duration_id:16;
	uint8_t addr1[6]; /* receiver address */
	uint8_t addr2[6]; /* sender address */
	uint8_t addr3[6]; /* filtering address */
	unsigned sequence_ctrl:16;
	uint8_t addr4[6]; /* optional */
} wifi_ieee80211_mac_hdr_t;

typedef struct {
	wifi_ieee80211_mac_hdr_t hdr;
	uint8_t payload[0]; /* network data ended with 4 bytes csum (CRC32) */
} wifi_ieee80211_packet_t;

SemaphoreHandle_t wifi_semaphore;

static void wifi_sniffer_packet_handler(void *buff, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buff;
    wifi_ieee80211_packet_t *ieeepkt = (wifi_ieee80211_packet_t*)pkt->payload;
    wifi_ieee80211_mac_hdr_t *ieeehdr = &ieeepkt->hdr;

    if (type == WIFI_PKT_MISC) return;

    xSemaphoreTake(wifi_semaphore, 1000 / portTICK_RATE_MS);

    //uint8_t *mac_to;
    uint8_t *mac_from;

    //mac_to = ieeehdr->addr1;
    mac_from = ieeehdr->addr2;

    //printf("%02X:%02X:%02X:%02X:%02X:%02X\t\t%02X:%02X:%02X:%02X:%02X:%02X\n", mac_from[0], mac_from[1], mac_from[2], mac_from[3], mac_from[4], mac_from[5], mac_to[0], mac_to[1], mac_to[2], mac_to[3], mac_to[4], mac_to[5]);

    int c = stations.size();

    int idx = -1;

    for (int i = 0; i < c; ++i) {
        if(memcmp(stations.get(i).mac, mac_from, 6) == 0) {
            idx = i;
            break;
        }
    }

    if (idx == -1) {
        station_t new_station;
        new_station.ap = 0;
        new_station.ch = pkt->rx_ctrl.channel;
        new_station.mac = (uint8_t*)malloc(6);
        new_station.pkts = (uint32_t*)calloc(3, sizeof(uint32_t));
        new_station.rssi = (signed int*)malloc(sizeof(signed int));
        new_station.time = (uint64_t*)malloc(sizeof(uint64_t));

        *new_station.rssi = 0;
        
        *new_station.time = esp_timer_get_time();

        memcpy(new_station.mac, mac_from, 6);
        new_station.pkts[type] = 1;

        stations.add(new_station);
    } else {
        (stations.get(idx).pkts[type]) += 1;
        *(stations.get(idx).rssi) = pkt->rx_ctrl.rssi;
        *(stations.get(idx).time) = esp_timer_get_time();
    }

    xSemaphoreGive(wifi_semaphore);
}

void change_channel(int dir) {
    bool sniffing;
    esp_wifi_get_promiscuous(&sniffing);
    if (sniffing) {
        uint8_t channel;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&channel, &second);

        channel += dir;

        if (channel > 14) channel = 1;
        if (channel < 1) channel = 14;

        esp_wifi_set_channel(channel, second);
    }
}

void clear_stations() {
    xSemaphoreTake(wifi_semaphore, 1000 / portTICK_RATE_MS);
    int c = stations.size();
    for (int i = 0; i < c; ++i) {
        free(stations.get(i).mac);
        free(stations.get(i).pkts);
        free(stations.get(i).rssi);
        free(stations.get(i).time);
    }
    stations.clear();
    xSemaphoreGive(wifi_semaphore);
}

extern "C" {
   void app_main();
}

void app_main(void)
{
    nvs_flash_init();
    tcpip_adapter_init();
    
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_scan_config_t scan_conf;

	scan_conf.show_hidden = true;

    printf("free heap: %d\n", esp_get_free_heap_size());

    for(int i = 0; i < 4; ++i) {
    	fb[i] = (uint16_t*)malloc(320 * 240 *2 / 4);
    	if(!fb[i]) {
    		printf("failed to allocate fb%d!\n", i);
    		abort();
    	}
    }
    printf("free heap: %d\n", esp_get_free_heap_size());
    //abort();

    odroid_input_gamepad_init();
    odroid_input_battery_level_init();


    // turn LED on
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);

    gpio_set_level(GPIO_NUM_2, 0);


    ili9341_init();
    ili9341_clear(0xffff);

    UG_Init(&gui, pset, 320, 240);

    odroid_battery_state bat;

    //ui_update_display();
    //menu_main();
    UG_FontSelect(&FONT_8X8);
    UG_SetForecolor(C_WHITE);
    UG_SetBackcolor(C_MIDNIGHT_BLUE);


    ui_draw_title();

    odroid_gamepad_state previousState;
    odroid_input_gamepad_read(&previousState);

    int current_item = 0;
    uint16_t num_aps = 0;

    start_scan(&scan_conf);

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };

    esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_packet_handler);
    esp_wifi_set_promiscuous_filter(&filter);
    bool sniffing = false;
    int items_per_page = 4;
    void (*draw_fct)(int, int, int, int);
    uint16_t max_items = 0;
    wifi_semaphore = xSemaphoreCreateMutex();

    while(1)
    {
		odroid_gamepad_state state;
		odroid_input_gamepad_read(&state);
        esp_wifi_get_promiscuous(&sniffing);

        if(!sniffing) {
            max_items = num_aps;
            draw_fct = &print_ap_info;
            items_per_page = ITEMS_APS;
        } else {
            max_items = stations.size();
            draw_fct = &print_sta_info;
            items_per_page = ITEMS_STAS;
        }

		if (!previousState.values[ODROID_INPUT_MENU] && state.values[ODROID_INPUT_MENU]) {
			ui_draw_title();
			DisplayMessage("rebooting...");
			vTaskDelay(1000 / portTICK_PERIOD_MS);

			boot_application();
		}
        
        if (!previousState.values[ODROID_INPUT_B] && state.values[ODROID_INPUT_B]) {
            if (sniffing) {
                
                clear_stations();              

                current_item = 0;

                UG_FontSelect(&FONT_8X8);
                UG_SetForecolor(C_WHITE);
                UG_SetBackcolor(C_MIDNIGHT_BLUE);
                UG_PutString(4, 240 - 4 - 8, "cleared list            ");
                last_time = esp_timer_get_time();
                ui_update_display();
                //draw_page(max_items, current_item, items_per_page, draw_fct);
            }
        }

		if (!previousState.values[ODROID_INPUT_START] && state.values[ODROID_INPUT_START] && !scan_finished) {
            esp_wifi_set_promiscuous(false);            
            clear_stations();
            ui_update_display();
            start_scan(&scan_conf);
		}

        if (!previousState.values[ODROID_INPUT_A] && state.values[ODROID_INPUT_A]) {
            uint8_t channel = num_aps ? ap[current_item].primary : 1;
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            esp_wifi_set_promiscuous(true);
        }

        if (!previousState.values[ODROID_INPUT_RIGHT] && state.values[ODROID_INPUT_RIGHT]) {
            change_channel(1);
        }

        if (!previousState.values[ODROID_INPUT_LEFT] && state.values[ODROID_INPUT_LEFT]) {
            change_channel(-1);
        }

		if (!previousState.values[ODROID_INPUT_UP] && state.values[ODROID_INPUT_UP]) {
            current_item--;
            if (current_item < 0) current_item = max_items - 1;
			draw_page(max_items, current_item, items_per_page, draw_fct);
		}

		if (!previousState.values[ODROID_INPUT_DOWN] && state.values[ODROID_INPUT_DOWN]) {
            current_item++;
            if (current_item > max_items - 1) current_item = 0;
			draw_page(max_items, current_item, items_per_page, draw_fct);
		}


		if (scan_finished == 2) {
			scan_finished = 0;
			gpio_set_level(GPIO_NUM_2, 0);
			esp_wifi_scan_get_ap_num(&num_aps);

			if (current_item >= num_aps) current_item = num_aps - 1;

            if (ap) {
                free(ap);
                ap = NULL;
            }
            ap = (wifi_ap_record_t*)malloc(num_aps * sizeof(wifi_ap_record_t));
            memset(ap, 0, num_aps * sizeof(wifi_ap_record_t));
            uint16_t num = num_aps;
            esp_wifi_scan_get_ap_records(&num, ap);

            for (int i = 0; i < num_aps; ++i) {
            	printf("%s, %d, %02X:%02X:%02X:%02X:%02X:%02X\n", ap[i].ssid, ap[i].rssi, ap[i].bssid[0], ap[i].bssid[1], ap[i].bssid[2], ap[i].bssid[3], ap[i].bssid[4], ap[i].bssid[5]);
            }
            
			draw_page(num_aps, current_item, ITEMS_APS, &print_ap_info);
            UG_FontSelect(&FONT_8X8);
            UG_SetForecolor(C_WHITE);
            UG_SetBackcolor(C_MIDNIGHT_BLUE);
            UG_PutString(4, 240 - 4 - 8, "idle                  ");
            ui_update_display();

            if(scanning) start_scan(&scan_conf);
		}

        if (last_time < esp_timer_get_time() - 500000)
        {
        	last_time = esp_timer_get_time();

            esp_wifi_get_promiscuous(&sniffing);

            if (sniffing) {
                draw_page(stations.size(), current_item, ITEMS_STAS, print_sta_info);
                UG_FontSelect(&FONT_8X8);
                UG_SetForecolor(C_WHITE);
                UG_SetBackcolor(C_MIDNIGHT_BLUE);
                uint8_t channel;
                wifi_second_chan_t second;
                esp_wifi_get_channel(&channel, &second);
                sprintf(tempstring, "sniffing on ch %u       ", channel);
                UG_PutString(4, 240 - 4 - 8, tempstring);
            }

            odroid_input_battery_level_read(&bat);
            uint32_t free_heap = esp_get_free_heap_size();

            UG_FontSelect(&FONT_8X8);
            UG_SetForecolor(C_WHITE);
            UG_SetBackcolor(C_MIDNIGHT_BLUE);

            sprintf(tempstring, "%d mV", bat.millivolts);
            UG_PutString(240, 4, tempstring);

            sprintf(tempstring, "heap: %6u", free_heap);
            UG_PutString(100, 4, tempstring);

            ui_update_display();
        }

        previousState = state;
    	vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    //indicate_error();
}
