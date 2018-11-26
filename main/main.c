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

//#include "odroid_sdcard.h"
#include "odroid_display.h"
#include "odroid_input.h"

#include "../components/ugui/ugui.h"
#include "oui.h"


const char* SD_CARD = "/sd";
//const char* HEADER = "ODROIDGO_FIRMWARE_V00_00";
const char* HEADER_V00_01 = "ODROIDGO_FIRMWARE_V00_01";

#define FIRMWARE_DESCRIPTION_SIZE (40)
char FirmwareDescription[FIRMWARE_DESCRIPTION_SIZE];

// <partition type=0x00 subtype=0x00 label='name' flags=0x00000000 length=0x00000000>
// 	<data length=0x00000000>
// 		[...]
// 	</data>
// </partition>
typedef struct
{
    uint8_t type;
    uint8_t subtype;
    uint8_t _reserved0;
    uint8_t _reserved1;

    uint8_t label[16];

    uint32_t flags;
    uint32_t length;
} odroid_partition_t;

// ------

static uint16_t *fb[4];

UG_GUI gui;
char tempstring[512];
char vendorstring[64];

#define ITEM_COUNT (4)
char** files;
int fileCount;
const char* path = "/sd/odroid/firmware";
char* VERSION = NULL;

uint64_t last_time = 0;

#define TILE_WIDTH (86)
#define TILE_HEIGHT (48)
#define TILE_LENGTH (TILE_WIDTH * TILE_HEIGHT * 2)
//uint8_t TileData[TILE_LENGTH];


static void pset(UG_S16 x, UG_S16 y, UG_COLOR color)
{
	assert(x < 320);
	assert(y < 240);
    fb[y / 60][(y % 60) * 320 + x] = color;
}

static void ui_update_display()
{
    ili9341_write_frame_rectangleLE(0, 0, 320, 60, fb[0]);
    ili9341_write_frame_rectangleLE(0, 60, 320, 60, fb[1]);
    ili9341_write_frame_rectangleLE(0, 120, 320, 60, fb[2]);
    ili9341_write_frame_rectangleLE(0, 180, 320, 60, fb[3]);
}

static void UpdateDisplay()
{
    ui_update_display();
}

static void DisplayMessage(const char* message)
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
    const char* TITLE = "ODROID-GO";

    UG_FillFrame(0, 0, 319, 239, C_WHITE);

    // Header
    UG_FillFrame(0, 0, 319, 15, C_MIDNIGHT_BLUE);
    UG_FontSelect(&FONT_8X8);
    const short titleLeft = (320 / 2) - (strlen(TITLE) * 9 / 2);
    UG_SetForecolor(C_WHITE);
    UG_SetBackcolor(C_MIDNIGHT_BLUE);
    UG_PutString(4, 4, TITLE);

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

#define ITEMS_PER_PAGE 4

wifi_ap_record_t *ap;

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

int binSearchVendors(uint8_t* searchBytes, int lowerEnd, int upperEnd) {
    uint8_t listBytes[3];
    int     res;
    int     mid = (lowerEnd + upperEnd) / 2;

    while (lowerEnd <= upperEnd) {
        listBytes[0] = (*(const unsigned char *)(data_macs + mid * 5));
        listBytes[1] = (*(const unsigned char *)(data_macs + mid * 5 + 1));
        listBytes[2] = (*(const unsigned char *)(data_macs + mid * 5 + 2));

        res = memcmp(searchBytes, listBytes, 3);

        if (res == 0) {
            return mid;
        } else if (res < 0) {
            upperEnd = mid - 1;
            mid      = (lowerEnd + upperEnd) / 2;
        } else if (res > 0) {
            lowerEnd = mid + 1;
            mid      = (lowerEnd + upperEnd) / 2;
        }
    }

    return -1;
}

void searchVendor(uint8_t* mac) {
    int    pos        = binSearchVendors(mac, 0, sizeof(data_macs) / 5 - 1);
    int    realPos    = (*(const unsigned char *)(data_macs + pos * 5 + 3)) | (*(const unsigned char *)(data_macs + pos * 5 + 4)) << 8;
    int idx = 0;
    if (pos >= 0) {
        char tmp;

        for (int i = 0; i < 8; i++) {
            tmp = (char)(*(const unsigned char *)(data_vendors + realPos * 8 + i));

            if (tmp != 0) vendorstring[idx++] += tmp;
            tmp += ' ';
        }
    }
    vendorstring[idx] = 0;
}

void draw_page(uint32_t num_items, uint32_t current_item) {
	const int innerHeight = 240 - (16 * 2); // 208
	const int itemHeight = innerHeight / ITEMS_PER_PAGE; // 52

	const int rightWidth = (213); // 320 * (2.0 / 3.0)

	const short textLeft = 4;

	int page = current_item / ITEMS_PER_PAGE;
	page *= ITEM_COUNT;

	UG_FillFrame(0, 15, 319, 222, C_WHITE);
    UG_FontSelect(&FONT_6X8);

	if (num_items > 0) {
		for (int line = 0; line < ITEMS_PER_PAGE; ++line) {
			if (page + line >= num_items) break;

            short top = 16 + (line * itemHeight) - 1;

	        if ((page) + line == current_item)
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_YELLOW);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 1 - 1, C_YELLOW);
	        }
	        else
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_WHITE);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 1 - 1, C_WHITE);
	        }

	        wifi_ap_record_t *cur_ap = ap + page + line;
	        char country[3] = {cur_ap->country.cc[0], cur_ap->country.cc[1], 0};
	        if (country[0] < 'A' || country[0] > 'Z') {
	        	country[0] = '-';
	        	country[1] = '-';
	        }

	        memset(vendorstring, 0, 64);
	        searchVendor(cur_ap->bssid);

	        sprintf(tempstring, "SSID: %s\nCH: %2d, RSSI: %3ddb, %s, %s\nPairC: %s, GroupC: %s\n%02X:%02X:%02X:%02X:%02X:%02X, Vendor: %s", cur_ap->ssid, cur_ap->primary, cur_ap->rssi, country, wifi_auth_types[cur_ap->authmode], wifi_cipher_types[cur_ap->pairwise_cipher], wifi_cipher_types[cur_ap->group_cipher], cur_ap->bssid[0], cur_ap->bssid[1], cur_ap->bssid[2], cur_ap->bssid[3], cur_ap->bssid[4], cur_ap->bssid[5], vendorstring);	        
	        UG_PutString(textLeft, top + 6, tempstring);
		}

        sprintf(tempstring, "       %d/%d", current_item + 1, num_items);
        UG_SetForecolor(C_WHITE);
        UG_SetBackcolor(C_MIDNIGHT_BLUE);
        UG_PutString(320 - strlen(tempstring) * 8 - 10, 240 - 4 - 8, tempstring);

		ui_update_display();
	}
}

void start_scan(wifi_scan_config_t *scan_conf) {
    UG_FontSelect(&FONT_8X8);
    UG_SetForecolor(C_WHITE);
    UG_SetBackcolor(C_MIDNIGHT_BLUE);

    UG_PutString(4, 240 - 4 - 8, "scanning wifi");

    ui_update_display();
    scan_finished = 1;
    gpio_set_level(GPIO_NUM_2, 1);
    ESP_ERROR_CHECK(esp_wifi_scan_start(scan_conf, false));
}

void app_main(void)
{
    const char* VER_PREFIX = "Ver: ";
    size_t ver_size = strlen(VER_PREFIX) + strlen(COMPILEDATE) + 1 + strlen(GITREV) + 1;
    VERSION = malloc(ver_size);
    if (!VERSION) abort();

    strcpy(VERSION, VER_PREFIX);
    strcat(VERSION, COMPILEDATE);
    strcat(VERSION, "-");
    strcat(VERSION, GITREV);

    printf("odroid-go-firmware (%s). HEAP=%d\n", VERSION, esp_get_free_heap_size());

    nvs_flash_init();
    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_scan_config_t scan_conf = {
    		.ssid = NULL,
			.bssid = NULL,
			.channel = NULL,
			.show_hidden = true,
    };
    printf("free heap: %d\n", esp_get_free_heap_size());

    for(int i = 0; i < 4; ++i) {
    	fb[i] = malloc(320 * 240 *2 / 4);
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

    while(1)
    {
		odroid_gamepad_state state;
		odroid_input_gamepad_read(&state);

		if (!previousState.values[ODROID_INPUT_MENU] && state.values[ODROID_INPUT_MENU]) {
			ui_draw_title();
			DisplayMessage("rebooting...");
			vTaskDelay(1000 / portTICK_PERIOD_MS);

			boot_application();
		}

		if (!previousState.values[ODROID_INPUT_A] && state.values[ODROID_INPUT_A] && !scan_finished) {
            ui_update_display();
            start_scan(&scan_conf);
		}

		if (!previousState.values[ODROID_INPUT_START] && state.values[ODROID_INPUT_START]) {
            UG_FontSelect(&FONT_8X8);
            UG_SetForecolor(C_WHITE);
            UG_SetBackcolor(C_MIDNIGHT_BLUE);

            if (scanning) {
                scanning = 0;
                UG_PutString(4, 240 - 4 - 8, "idle                  ");
            } else {
                scanning = 1;
                UG_PutString(4, 240 - 4 - 8, "scanning              ");
                start_scan(&scan_conf);
            }
            ui_update_display();
		}

		if (!previousState.values[ODROID_INPUT_UP] && state.values[ODROID_INPUT_UP]) {
			if (current_item > 0) current_item--;
			else current_item = num_aps - 1;
			draw_page(num_aps, current_item);
		}

		if (!previousState.values[ODROID_INPUT_DOWN] && state.values[ODROID_INPUT_DOWN]) {
			if (current_item < num_aps - 1) current_item++;
			else current_item = 0;
			draw_page(num_aps, current_item);
		}


		if (scan_finished == 2) {
			scan_finished = 0;
			gpio_set_level(GPIO_NUM_2, 0);
			esp_wifi_scan_get_ap_num(&num_aps);

			if (current_item > num_aps) current_item = num_aps - 1;

            if (ap) {
                free(ap);
                ap = NULL;
            }
            ap = malloc(num_aps * sizeof(wifi_ap_record_t));
            memset(ap, 0, num_aps * sizeof(wifi_ap_record_t));
            uint16_t num = num_aps;
            esp_wifi_scan_get_ap_records(&num, ap);

            for (int i = 0; i < num_aps; ++i) {
            	printf("%s, %d, %02X:%02X:%02X:%02X:%02X:%02X\n", ap[i].ssid, ap[i].rssi, ap[i].bssid[0], ap[i].bssid[1], ap[i].bssid[2], ap[i].bssid[3], ap[i].bssid[4], ap[i].bssid[5]);
            }
            
			draw_page(num_aps, current_item);
            UG_FontSelect(&FONT_8X8);
            UG_SetForecolor(C_WHITE);
            UG_SetBackcolor(C_MIDNIGHT_BLUE);
            UG_PutString(4, 240 - 4 - 8, "idle                  ");
            ui_update_display();

            if(scanning) start_scan(&scan_conf);
		}

        if (last_time < esp_timer_get_time() - 1000000)
        {
        	last_time = esp_timer_get_time();
            odroid_input_battery_level_read(&bat);

            sprintf(tempstring, "%d mV", bat.millivolts);

            UG_FontSelect(&FONT_8X8);
            UG_SetForecolor(C_WHITE);
            UG_SetBackcolor(C_MIDNIGHT_BLUE);

            UG_PutString(240, 4, tempstring);

            ui_update_display();
        }

        previousState = state;
    	vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    //indicate_error();
}
