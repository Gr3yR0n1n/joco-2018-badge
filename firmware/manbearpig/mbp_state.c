/*****************************************************************************
 * (C) Copyright 2017 AND!XOR LLC (http://andnxor.com/).
 *
 * PROPRIETARY AND CONFIDENTIAL UNTIL AUGUST 1ST, 2017 then,
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Contributors:
 * 	@andnxor
 * 	@zappbrandnxor
 * 	@hyr0n1
 * 	@andrewnriley
 * 	@lacosteaef
 * 	@bitstr3m
 *****************************************************************************/

#include "system.h"

#define CANARY				(0x19)
#define ECB_KEY_LEN          		(16UL)
#define COUNTER_BYTE_LEN     		(4UL)
#define NONCE_RAND_BYTE_LEN   		(12UL)

#define STATE_FILE_PATH			"SHADOW.DAT"

uint8_t key[ECB_KEY_LEN] = {
		0x4d, 0x92, 0x91, 0x63,
		0x8b, 0x4b, 0x46, 0xd0,
		0x67, 0x8d, 0x67, 0x53,
		0xa3, 0xc8, 0xf0, 0x01
};

/**
 * Struct used to maintain the CBC state for the encrypted badge state
 * Useful for serialization and deserialization from a file.
 */
typedef struct {
	uint32_t counter;
	uint8_t nonce[NONCE_RAND_BYTE_LEN];
	uint8_t cipher_text[sizeof(badge_state_t)];
} cbc_badge_state_t;

// NOTE: The ECB data must be located in RAM or a HardFault will be triggered.
static nrf_ecb_hal_data_t m_ecb_data;
static bool m_initialized = false;

//Encryption status
//TODO: Determine if this is necessary to store globally (might not be)
static cbc_badge_state_t m_cbc_state;
//Badge State, global storage
static badge_state_t m_badge_state;

/**
 * @brief Uses the RNG to write a 12-byte nonce to a buffer
 * @details The 12 bytes will be written to the buffer starting at index 4 to leave
 *          space for the 4-byte counter value.
 *
 * @param[in]    p_buf    An array of length 16
 */
void __nonce_generate(uint8_t * p_buf) {
	uint8_t i = 0;
	uint8_t remaining = NONCE_RAND_BYTE_LEN;

// The random number pool may not contain enough bytes at the moment so
// a busy wait may be necessary.
	while (remaining > 0) {
		p_buf[i] = util_math_rand8();
		i++;
		remaining--;
	}
}

/**
 * @brief Initializes the module with the given nonce and key. This should only be called the
 * first time with the given nonce and key. Otherwise the counter will be re-used and security
 * will be compromised.
 *
 * @details The nonce will be copied to an internal buffer so it does not need to
 *          be retained after the function returns. Additionally, a 32-bit counter
 *          will be initialized to zero and placed into the least-significant 4 bytes
 *          of the internal buffer. The nonce value should be generated in a
 *          reasonable manner (e.g. using this module's nonce_generate function).
 *
 * @param[in]    p_nonce    An array of length 16 containing 12 random bytes
 *                          starting at index 4
 * @param[in]    p_ecb_key  An array of length 16 containing the ECB key
 */
void __ctr_init() {
	m_initialized = true;

	uint16_t serial = util_get_device_id();
	key[14] = (serial >> 8) & 0x00FF;
	key[15] = serial & 0x00FF;

	// Save the key.
	memcpy(&m_ecb_data.key[0], key, ECB_KEY_LEN);

	// Copy the noncef from global
	memcpy(&m_ecb_data.cleartext[COUNTER_BYTE_LEN], m_cbc_state.nonce, NONCE_RAND_BYTE_LEN);

	//Init counter value using the current counter from global
	(*((uint32_t*) m_ecb_data.cleartext)) = m_cbc_state.counter;
}

static uint32_t __crypt(uint8_t * buf, uint16_t length) {
	uint32_t err_code;
	uint8_t block_size;
	uint16_t offset = 0;

	if (!m_initialized) {
		return NRF_ERROR_INVALID_STATE;
	}

	//Iterate through all blocks of data
	while (length > 0) {

		//Determine how big of a block to do, limited by ECB_KEY_LEN
		//This ensures we don't run off the end of the memory and start encrypting stuff we shouldnt
		block_size = MIN(length, ECB_KEY_LEN);

		//Do the hardware encryption by generating a block of ciphertext, quit with any failure
		err_code = sd_ecb_block_encrypt(&m_ecb_data);
		if (NRF_SUCCESS != err_code) {
			return err_code;
		}

		//XOR the private data with the chunk of ciphertext
		for (uint8_t i = 0; i < block_size; i++) {
			uint8_t v = buf[offset] ^ m_ecb_data.ciphertext[i];
			buf[offset] = v;
			offset++;
		}

		// Increment the counter.
		(*((uint32_t*) m_ecb_data.cleartext))++;

		//Less work to do
		length -= block_size;
	}

	return NRF_SUCCESS;
}

/**
 * @brief Encrypts the given buffer in-situ
 * @details The encryption step is done separately (using the nonce, counter, and
 *          key) and then the result from the encryption is XOR'd with the given
 *          buffer in-situ. The counter will be incremented only if no error occurs.
 *
 * @param[in]	p_clear_text	An array of length 16 containing the clear text
 * @param[out]	length			Length of the cipher text to be encrypted
 *
 * @retval    NRF_SUCCESS                         Success
 * @retval    NRF_ERROR_INVALID_STATE             Module has not been initialized
 * @retval    NRF_ERROR_SOFTDEVICE_NOT_ENABLED    SoftDevice is present, but not enabled
 */
uint32_t __ctr_encrypt(uint8_t * p_clear_text, uint16_t length) {
	return __crypt(p_clear_text, length);
}

/**
 * @brief Decrypts the given buffer in-situ
 * @details The encryption step is done separately (using the nonce, counter, and
 *          key) and then the result from the encryption is XOR'd with the given
 *          buffer in-situ. The counter will be incremented only if no error occurs.
 *
 * @param[in]	p_cipher_text	An array of length 16 containing the cipher text
 * @param[out]	length			Length of the cipher text to be decrypted
 *
 * @retval    NRF_SUCCESS                         Succeess
 * @retval    NRF_ERROR_INVALID_STATE             Module has not been initialized
 * @retval    NRF_ERROR_SOFTDEVICE_NOT_ENABLED    SoftDevice is present, but not enabled
 */
uint32_t __ctr_decrypt(uint8_t * p_cipher_text, uint16_t length) {
	return __crypt(p_cipher_text, length);
}

void mbp_state_new() {
	memcpy(&(m_badge_state.name), SETTING_NAME_DEFAULT, SETTING_NAME_LENGTH);
	util_ble_name_set(m_badge_state.name);

	m_badge_state.airplane_mode_enabled = SETTING_AIRPLANE_MODE_DEFAULT;
	m_badge_state.tilt_enabled = SETTING_TILT_ENABLED_DEFAULT;
	m_badge_state.canary = CANARY;
	m_badge_state.game_exit_pop_up = SETTING_GAME_EXIT_POPUP_DEFAULT;
	m_badge_state.game_led_sound = SETTING_GAME_LED_SOUND_DEFAULT;
	m_badge_state.chip8_fg_color = SETTING_CHIP8_FG_COLOR_DEFAULT;
	m_badge_state.chip8_bg_color = SETTING_CHIP8_BG_COLOR_DEFAULT;
	m_badge_state.unlock_state = SETTING_UNLOCK_DEFAULT;
	m_badge_state.master_badge = SETTING_MASTER_DEFAULT;
	m_badge_state.joco_score = GAME_SCORE_DEFAULT;
	m_badge_state.joco_last_level_dispensed = GAME_LASTLEVEL_DEFAULT;

	strcpy(m_badge_state.pw_scruffy, "54321");
	strcpy(m_badge_state.pw_root, "hamneggs");

	strcpy(m_badge_state.wall_messages[0], "Msg 1 None");
	strcpy(m_badge_state.wall_messages[1], "Msg 2 None");
	strcpy(m_badge_state.wall_messages[2], "Msg 3 None");
	strcpy(m_badge_state.wall_messages[3], "Msg 4 None");
	strcpy(m_badge_state.wall_messages[4], "Msg 5 None");
	m_badge_state.wall_current_spot = 0;

	//Reset the counter to 0
	m_cbc_state.counter = 0;
}

bool mbp_state_load() {
	uint32_t err_code;
	FIL file;
	FILINFO info;
	FRESULT result;
	UINT count;

	//check if file exists, if not create it
	if (f_stat(STATE_FILE_PATH, &info) != FR_OK) {
		mbp_state_new();
		mbp_state_save();
	}

	//Open settings file
	result = f_open(&file, STATE_FILE_PATH, FA_READ | FA_OPEN_EXISTING);
	if (result != FR_OK) {
		return false;
	}

	result = f_read(&file, (void *) &m_cbc_state, sizeof(cbc_badge_state_t), &count);
	if (result != FR_OK) {
		mbp_ui_error("Could not read settings file.");
		return false;
	}

	result = f_close(&file);
	if (result != FR_OK) {
		mbp_ui_error("Could not close settings file.");
		return false;
	}

	__ctr_init();

	err_code = __ctr_decrypt(m_cbc_state.cipher_text, sizeof(badge_state_t));
	APP_ERROR_CHECK(err_code);

	if (err_code != NRF_SUCCESS) {
		mbp_ui_error("Could not decrypt settings file.");
		return false;
	}

	m_cbc_state.counter = (*((uint32_t*) m_ecb_data.cleartext));

	memcpy(&m_badge_state, m_cbc_state.cipher_text, sizeof(badge_state_t));

	if ((m_badge_state.canary == CANARY)) {
		util_ble_name_set(m_badge_state.name);
		util_ble_score_update();
		return true;
	}

	return false;
}

static void __save_schedule_handler(void *p_data, uint16_t length) {
	uint32_t err_code;

	//Generate a new nonce
	__nonce_generate(m_cbc_state.nonce);

	//Setup encryption parameters
	__ctr_init(key);

	//Serialize the state into raw bytes
	memcpy(m_cbc_state.cipher_text, &m_badge_state, sizeof(badge_state_t));

	//Encrypt the data
	err_code = __ctr_encrypt(m_cbc_state.cipher_text, sizeof(badge_state_t));
	APP_ERROR_CHECK(err_code);

	//Write the data to SD
	FIL file;
	FRESULT result;
	UINT count;
	result = f_open(&file, STATE_FILE_PATH, FA_CREATE_ALWAYS | FA_WRITE);
	if (result != FR_OK) {
		mbp_ui_error("Could not open settings for writing.");
		return;
	}

	result = f_write(&file, (void *) &m_cbc_state, sizeof(cbc_badge_state_t), &count);
	if (result != FR_OK) {
		mbp_ui_error("Could not write to settings file.");
	}

	result = f_close(&file);
	if (result != FR_OK) {
		mbp_ui_error("Could not close settings file.");
	}

	//Since save is complete, update counter into global CBC data for next save
	m_cbc_state.counter = (*((uint32_t*) m_ecb_data.cleartext));
}

void mbp_state_save() {
	app_sched_event_put(NULL, 0, __save_schedule_handler);
}

bool mbp_state_airplane_mode_get() {
	return m_badge_state.airplane_mode_enabled;
}

void mbp_state_airplane_mode_set(bool enabled) {
	m_badge_state.airplane_mode_enabled = enabled;
}

void mbp_state_name_get(char *name) {
	snprintf(name, SETTING_NAME_LENGTH, "%s", m_badge_state.name);
}

void mbp_state_name_set(char *name) {
	snprintf(m_badge_state.name, SETTING_NAME_LENGTH, "%s", name);
	util_ble_name_set(name);
}

bool mbp_state_game_led_sound_get() {
	return m_badge_state.game_led_sound;
}

void mbp_state_game_led_sound_set(bool b) {
	m_badge_state.game_led_sound = b;
}

bool mbp_state_game_exit_pop_up_get() {
	//gets the current state of the CHIP8 "How to Exit" pop up
	return m_badge_state.game_exit_pop_up;
}

void mbp_state_game_exit_pop_up_set(bool b) {
	//sets the current state of the CHIP8 "How to Exit" pop up
	m_badge_state.game_exit_pop_up = b;
}

uint16_t mbp_state_chip8_fg_color_get() {
	//gets the current state of the CHIP8 foreground color
	return m_badge_state.chip8_fg_color;
}

void mbp_state_chip8_fg_color_set(uint16_t c) {
	//sets the current state of the CHIP8 foreground color
	m_badge_state.chip8_fg_color = c;
}

uint16_t mbp_state_chip8_bg_color_get() {
	//gets the current state of the CHIP8 background color
	return m_badge_state.chip8_bg_color;
}

void mbp_state_chip8_bg_color_set(uint16_t c) {
	//sets the current state of the CHIP8 background color
	m_badge_state.chip8_bg_color = c;
}

bool mbp_state_master_get() {
	return m_badge_state.master_badge;
}

void mbp_state_master_set(bool master) {
	m_badge_state.master_badge = master;
}

bool mbp_state_tilt_get() {
	return m_badge_state.tilt_enabled;
}

void mbp_state_tilt_set(bool tilt_state) {
	m_badge_state.tilt_enabled = tilt_state;
}

uint16_t mbp_state_unlock_get() {
	return m_badge_state.unlock_state;
}

void mbp_state_unlock_set(uint16_t unlock_state) {
	m_badge_state.unlock_state = unlock_state;
}

uint16_t mbp_state_score_get() {
	return m_badge_state.joco_score;
}

void mbp_state_score_set(uint16_t score_state) {
	m_badge_state.joco_score = score_state;
	util_ble_score_update();
}

uint8_t mbp_state_lastlevel_get() {
	return m_badge_state.joco_last_level_dispensed;
}

void mbp_state_lastlevel_set(uint8_t lastlevel_state) {
	m_badge_state.joco_last_level_dispensed = lastlevel_state;
}

void mbp_state_pw_scruffy_set(char *pw) {
	snprintf(m_badge_state.pw_scruffy, SETTING_PW_LENGTH, "%s", pw);
}

void mbp_state_pw_scruffy_get(char *pw) {
	snprintf(pw, SETTING_PW_LENGTH, "%s", m_badge_state.pw_scruffy);
}

void mbp_state_pw_root_set(char *pw) {
	snprintf(m_badge_state.pw_root, SETTING_PW_LENGTH, "%s", pw);
}

void mbp_state_pw_root_get(char *pw) {
	snprintf(pw, SETTING_PW_LENGTH, "%s", m_badge_state.pw_root);
}

void mbp_state_wall_show(){
	/*todo v0.8 Polish
	char temp[16];
	for (int i = 0; i< 5; i++){
		sprintf(temp, "MSG%i ", i);
		strcat(temp, m_badge_state.wall_messages[i]);
		mbp_term_print(temp);
	}
	*/
	mbp_term_print(m_badge_state.wall_messages[0]);
	mbp_term_print(m_badge_state.wall_messages[1]);
	mbp_term_print(m_badge_state.wall_messages[2]);
	mbp_term_print(m_badge_state.wall_messages[3]);
	mbp_term_print(m_badge_state.wall_messages[4]);
	mbp_term_print("\r");
}

extern void mbp_state_wall_put(char *msg){
	snprintf(m_badge_state.wall_messages[m_badge_state.wall_current_spot], 16, "%s", msg);

	if(m_badge_state.wall_current_spot < 4){
		m_badge_state.wall_current_spot++;
	}
	else{
		m_badge_state.wall_current_spot = 0;
	}

	mbp_state_save();
}

// Functions for saving and reading a simple 'database' of other badges we've visited.
