#include "hub75.h"
#include "pins.h"
#include "delay.h"
#include <math.h>
#include <string.h>

int hubBrightness = 50; //Brightness 2..100%; 0..1% - off
int hubMinBrightness = 2;

orient_t hubOrientation = HUB_ROTATE_0; //Display Orientation
char hubNeedRedraw = false;
	
screen_t matrix[SCREEN_PAGES];
screen_t * screen = &matrix[0], * display = &matrix[1], * screen_temp;

screen_t matrix[SCREEN_PAGES] = { 0 };

/* Connection HUB75 -> STM32F429I-DISCOVERY

	R1 ->	 PC11
	G1 ->	 PC12
	B1 ->	 PC13
	GND ->	 GND
	R2 ->	 PD2
	G2 ->	 PD4
	B2 ->	 PD5
	GND ->	 GND
	A  ->	 PE2
	B  ->	 PE3
	C  ->	 PE4
	D  ->	 PE5
	CLK ->	 PD7
	LAT ->	 PG2
	NOE ->	 PF6 (TIM10_CH1)
	GND ->	 GND

	PA5 ->	 pin for test timings
*/

const pin_t
	hub_r1 = {
		GPIOC,
		GPIO_PIN_11
},
	hub_g1 = {
		GPIOC,
		GPIO_PIN_12
},
	hub_b1 = {
		GPIOC,
		GPIO_PIN_13
},
	hub_r2 = {
		GPIOD,
		GPIO_PIN_2
},
	hub_g2 = {
		GPIOD,
		GPIO_PIN_4
},
	hub_b2 = {
		GPIOD,
		GPIO_PIN_5
},
	hub_a = {
		GPIOE,
		GPIO_PIN_2
},
	hub_b = {
		GPIOE,
		GPIO_PIN_3
},
	hub_c = {
		GPIOE,
		GPIO_PIN_4
},
	hub_d = {
		GPIOE,
		GPIO_PIN_5
},
	hub_clk = {
		GPIOD,
		GPIO_PIN_7
},
	hub_lat = {
		GPIOG,
		GPIO_PIN_2
},
	hub_noe = {
		GPIOF,
		GPIO_PIN_6
},
	hub_time = {
	GPIOA,
	GPIO_PIN_5
};
	
#define HUB_GPIO_CLK_ENABLE() \
	__GPIOC_CLK_ENABLE(); \
	__GPIOD_CLK_ENABLE(); \
	__GPIOE_CLK_ENABLE(); \
	__GPIOG_CLK_ENABLE(); \
	__GPIOF_CLK_ENABLE()

#define NOE_TIM_CHANNEL TIM_CHANNEL_1

TIM_HandleTypeDef hubtim, hubtim2;

static void initIO(void) {
	HUB_GPIO_CLK_ENABLE();
	
	GPIO_InitTypeDef gpioInit;

	gpioInit.Mode = GPIO_MODE_OUTPUT_PP;
	gpioInit.Pull = GPIO_PULLUP;
	gpioInit.Speed = GPIO_SPEED_HIGH;

	pinSet(hub_noe);
	gpioInit.Pin = hub_noe.pin;
	HAL_GPIO_Init(hub_noe.port, &gpioInit);
	pinSet(hub_noe);
	
	gpioInit.Pin = hub_r1.pin;
	HAL_GPIO_Init(hub_r1.port, &gpioInit);
	gpioInit.Pin = hub_b1.pin;
	HAL_GPIO_Init(hub_b1.port, &gpioInit);
	gpioInit.Pin = hub_g1.pin;
	HAL_GPIO_Init(hub_g1.port, &gpioInit);
	gpioInit.Pin = hub_r2.pin;
	HAL_GPIO_Init(hub_r2.port, &gpioInit);
	gpioInit.Pin = hub_g2.pin;
	HAL_GPIO_Init(hub_g2.port, &gpioInit);
	gpioInit.Pin = hub_b2.pin;
	HAL_GPIO_Init(hub_b2.port, &gpioInit);
	gpioInit.Pin = hub_a.pin;
	HAL_GPIO_Init(hub_a.port, &gpioInit);
	gpioInit.Pin = hub_b.pin;
	HAL_GPIO_Init(hub_b.port, &gpioInit);
	gpioInit.Pin = hub_c.pin;
	HAL_GPIO_Init(hub_c.port, &gpioInit);
	gpioInit.Pin = hub_d.pin;
	HAL_GPIO_Init(hub_d.port, &gpioInit);
	gpioInit.Pin = hub_clk.pin;
	HAL_GPIO_Init(hub_clk.port, &gpioInit);
	gpioInit.Pin = hub_lat.pin;
	HAL_GPIO_Init(hub_lat.port, &gpioInit);
	gpioInit.Pin = hub_time.pin;
	HAL_GPIO_Init(hub_time.port, &gpioInit);
	
	/**TIM10 GPIO Configuration    
   PF6     ------> TIM10_CH1 => NOE
  */
  gpioInit.Pin = GPIO_PIN_6;
  gpioInit.Mode = GPIO_MODE_AF_PP;
  gpioInit.Pull = GPIO_PULLUP;
  gpioInit.Speed = GPIO_SPEED_HIGH;
  gpioInit.Alternate = GPIO_AF3_TIM10;
  HAL_GPIO_Init(GPIOF, &gpioInit);
}

static int period0, period1, periodDark;

#define HUB_PRESCALER (HUB_TIMER_MHZ >> 1) // 500nS - for tick
#define FRAME_PERIOD (HUB_TIMER_MHZ * 1000000 / HUB_PRESCALER / HUB_UPDATE_HZ)
#define PERIOD0 (HUB_TIMER_MHZ * HUB_PERIOD0_US / HUB_PRESCALER)

static int prescaler2N[HUB_COLOR_DEPTH];
static int prescaler2n;
static int period1N[HUB_COLOR_DEPTH];
static int period1n;
static int period2Min, period2Max;

#define P1_KOEFF(x) (100 * (x) / 100 ) // equal to p2 

static inline void setBrightness(void) {
	int sum = 0, p1, p2, bit, k;
	if (hubBrightness < hubMinBrightness) hubBrightness = 0;
	else if (hubBrightness > 100) hubBrightness = 100;
	period2Max = HUB_PERIOD2_MAX_NS / HUB_TIMER2_NS * hubBrightness / 100;
	period2Min = HUB_PERIOD2_MIN_NS / HUB_TIMER2_NS;

	if (period1 < HUB_MIN_PERIOD) period1 = HUB_MIN_PERIOD;
	
	bit = HUB_COLOR_BIT0;
	k = (period2Max - period2Min) / (HUB_COLOR_BIT0 - 1);
	for (int i = HUB_COLOR_I0; i >= 0; i--) {
		p2 = k * (bit - 1) + period2Min;
		prescaler2N[i] = p2 / period2Min;
		p1 = P1_KOEFF(p2) / HUB_PRESCALER;
		if (p1 < HUB_MIN_PERIOD) p1 = HUB_MIN_PERIOD;
		period1N[i] = p1;
		sum += p1;
		bit >>= 1;
	}
	periodDark = FRAME_PERIOD - HUB_ROWS * (period0 * HUB_COLOR_DEPTH + sum);

	HUB_TIMER2->CCR1 = period2Min;
}

static void calculatePeriod(void) {
	period0 = PERIOD0;
	if (period0 < HUB_MIN_PERIOD) period0 = HUB_MIN_PERIOD;
	setBrightness();
}	

uint16_t hubLUT[HUB_LUT_NUM] = { 0 };
	
static float gamma(float br) {
	float res = 0.0f;
	if (br < 0.0f) br = 0.0f;
	else if (br > 1.0f) br = 1.0f;
	res = (float)pow(br, HUB_GAMMA);
	if (res < 0.0f) res = 0.0f;
	else if (res > 1.0f) res = 1.0f;
	return res;
}
void hubLUTInit(void) {
	float y;
	for (int i = 0; i < HUB_LUT_NUM; i++) {
		//hubLUT[i] = i; // 256-colors
		y = (HUB_COLOR_NUM - 1.5) * gamma((float)i / (HUB_LUT_NUM - 1)) + 0.5;
		hubLUT[i] = (int)y;
	}
}

const uint32_t testBmp[SCREEN_H * SCREEN_W] = { 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x607d54, 0x61794f, 0x63754f, 0x566940, 0x365024, 0x143106, 0x12d00, 0x4406, 0x5511, 0x186a23, 0x588946, 0x75a25e, 0x6aa35e, 0x3d7f38, 0x1d5d0b, 0x244c01, 0x294903, 0x3a5c0d, 0x38630b, 0x2d5202, 0x334409, 0x2f4414, 0x3b5c30, 0x4b734a, 0x59805b, 0x678565, 0x56774e, 0x6e5c34, 0x973823, 0xa22208, 0x973300, 0x785008, 0x646e28, 0x666928, 0x6b4418, 0x744b10, 0x687400, 0x498c00, 0x2d9c00, 0x2f9500, 0x427a00, 0x416c00, 0x316400, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x517447, 0x5c724e, 0x5f714c, 0x4b5f36, 0x244110, 0x2700, 0x2100, 0x2c00, 0x3800, 0x5200, 0x2b731a, 0x4d803b, 0x4f8741, 0x3a7e2f, 0x2c6414, 0x284a02, 0x253d00, 0x264900, 0x384e00, 0x574500, 0x663e0e, 0x5e3b18, 0x5a5030, 0x5c6c45, 0x5a7a54, 0x55815a, 0x4a7749, 0x5d6437, 0x7a4c25, 0x853c06, 0x743800, 0x574000, 0x5b5d00, 0x6d7c3e, 0x747b6a, 0x696a5f, 0x4e6a39, 0x348900, 0x2c9100, 0x328000, 0x397100, 0x3c6a00, 0x306900, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x48693d, 0x536a47, 0x516641, 0x3f5529, 0x243f0d, 0x73000, 0x2c00, 0x3500, 0x3d00, 0x4e00, 0x226604, 0x3c7524, 0x53843c, 0x4b823a, 0x31641c, 0x2a4305, 0x223400, 0x1f3b00, 0x584200, 0x994310, 0xad3f1b, 0xa52e16, 0x92321c, 0x825235, 0x5e643a, 0x326735, 0x3c683b, 0x3d5928, 0x383600, 0x543f00, 0x78682c, 0x847c5d, 0x827c6b, 0x696c67, 0x505e50, 0x405b34, 0x326518, 0x1f7900, 0x147800, 0x216d00, 0x2e6204, 0x326116, 0x3a7222, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x436130, 0x4b6438, 0x465e36, 0x3e542a, 0x2e4917, 0x203f04, 0x114100, 0x124b00, 0x285303, 0x305c0c, 0x3e6a1f, 0x537a38, 0x64844c, 0x587c3f, 0x3f6223, 0x294308, 0x143100, 0x2c3402, 0x823700, 0xcf2e00, 0xe53717, 0xd13c27, 0xc42913, 0xaf2003, 0x6e2c01, 0x1d3700, 0x3c03, 0x335522, 0x74866c, 0x767973, 0x676769, 0x5c5c55, 0x3a3f2c, 0x152702, 0x1b4400, 0x2e6b00, 0x2b6c00, 0x1c5e03, 0x135905, 0x19570b, 0x275a19, 0x336229, 0x436e36, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x3a551f, 0x415728, 0x3b5129, 0x3b4e28, 0x3b5021, 0x305110, 0x2c5907, 0x366110, 0x4d6c29, 0x526d32, 0x506d36, 0x5c7842, 0x657d4b, 0x5f7944, 0x4a6630, 0x2a4611, 0xa3401, 0x2e1900, 0x8b1e00, 0xad6355, 0x869492, 0x79b0a7, 0x90af9e, 0xa6987f, 0x908e6e, 0x528558, 0x5f8167, 0x888f9d, 0x767888, 0x3a3c38, 0x92200, 0x52a00, 0x153000, 0x1d350f, 0x21450a, 0x1e5103, 0x1c5109, 0x1a4c11, 0x184915, 0x1d4e18, 0x215719, 0x2b5c26, 0x3a5c33, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2e4e12, 0x324b17, 0x2c4618, 0x2f441b, 0x394e1c, 0x365610, 0x365c0a, 0x496b21, 0x5c7b3c, 0x5c7642, 0x577042, 0x5b7444, 0x5f7747, 0x5e7849, 0x4b6838, 0x294d1a, 0x181c00, 0x402a01, 0x62988d, 0xddcc, 0xc8a3, 0xaf83, 0xb384, 0xa88b, 0x2da28c, 0x568572, 0x404b51, 0x41272c, 0x340b00, 0x3b3200, 0x304f0f, 0x11450c, 0x33606, 0x112e0e, 0x153610, 0x163f0c, 0x1a4010, 0x1f3b18, 0x19411a, 0x144c17, 0x105012, 0x174715, 0x224320, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x284a0e, 0x24410d, 0x213e0f, 0x203c0e, 0x263c09, 0x2c4a00, 0x315a03, 0x436b21, 0x4f7835, 0x54733f, 0x576d42, 0x546e42, 0x577146, 0x547348, 0x43693f, 0x353d05, 0x375232, 0xd0c1, 0xf2be, 0x2aba53, 0x2a7e3d, 0x2d613e, 0x7400c, 0x1b07, 0x1327, 0x20525, 0x180a02, 0x762d01, 0x812c13, 0x674916, 0x406018, 0x17560d, 0x54105, 0x11330e, 0x183313, 0x1b3516, 0x1e3618, 0x1e361a, 0x1d401a, 0x1a4715, 0x124611, 0xf3710, 0x123012, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x224913, 0x213e12, 0x153206, 0x122b01, 0x142800, 0x1e3c00, 0x2c5905, 0x316415, 0x326722, 0x436b36, 0x536d41, 0x546c42, 0x516b43, 0x4c7146, 0x4e6230, 0x4a6b49, 0xd3bb, 0xe099, 0xb451, 0x16883c, 0x413934, 0x260616, 0xb0e07, 0x10e22, 0x1941, 0x112e, 0x643b16, 0x9a3807, 0x842408, 0x6b4515, 0x415f18, 0x1c5a0e, 0x144710, 0x183815, 0x173516, 0x1a3617, 0x1e361a, 0x1a3417, 0x143f15, 0x174a10, 0xd430b, 0xf330e, 0x162d14, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x25471d, 0x173c11, 0xd3203, 0x133200, 0x1a3400, 0x1e3901, 0x1a4a05, 0x195a13, 0x2d6629, 0x406d3b, 0x4e6d42, 0x556c43, 0x4b6a40, 0x4e6b3d, 0x5d6425, 0xb895, 0xe695, 0xa663, 0x7b78, 0x6765, 0x223944, 0x61f26, 0x3c70, 0x2869, 0x120e2a, 0x161619, 0x5a440e, 0x703300, 0x6b3101, 0x4f4407, 0x2c5109, 0x1d5010, 0x1e4916, 0x1b3f17, 0x1b3717, 0x1b3718, 0x1e391a, 0x1b3717, 0x123e0f, 0xa4b06, 0x34a06, 0x73c09, 0x143111, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x234220, 0x123910, 0x1b3f07, 0x244103, 0x2a4603, 0x2f4d12, 0x28511e, 0x28612b, 0x366a37, 0x3a6838, 0x43693c, 0x527146, 0x49703f, 0x426529, 0x435e30, 0x8c87, 0x8484, 0x5280, 0x4299, 0x131e61, 0x31f4d, 0x92256, 0x1c62, 0x3656, 0x2d34, 0x1e3e38, 0x4c6b23, 0x59661d, 0x646e32, 0x4d6c27, 0x1d5d0d, 0xe4b04, 0x1b440e, 0x1b4116, 0x193915, 0x1c3918, 0x1f3b1c, 0x1d3b19, 0x12420b, 0x65207, 0xf5609, 0x11450a, 0x103610, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x1a3818, 0x193d11, 0x2f4b0d, 0x384e0a, 0x3f5317, 0x425a29, 0x3f5a2e, 0x39622c, 0x396e37, 0x477846, 0x5a8758, 0x6b9469, 0x67905c, 0x4b7d37, 0x2e60aa, 0x6447e1, 0x7c3cd6, 0x4c24da, 0x2e0686, 0x105b, 0x2855, 0x5669, 0x776c, 0x6849, 0x5d45, 0x4645, 0x668c62, 0x84b16e, 0x80ac73, 0x6ea065, 0x4a8640, 0x1f6214, 0xd4507, 0x1a3c14, 0x163b16, 0x1d3d19, 0x1c3c18, 0x163f16, 0x144713, 0x1d5617, 0x235919, 0x184615, 0x174a1d, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x143512, 0x163304, 0x2d4500, 0x495a08, 0x455a11, 0x394f18, 0x2a4a19, 0x316632, 0x5f8f5c, 0x81ad7f, 0x9cba96, 0xa5bd93, 0x9db98b, 0x92b670, 0x52948b, 0x4f9b, 0x4394, 0xe78b4, 0x7f8c, 0xbda4, 0xd7b7, 0xcd89, 0xc576, 0x16140, 0x5741, 0x383d, 0x63937a, 0xb3da9c, 0x99c699, 0x92c190, 0x77ab73, 0x4d8445, 0x1a5617, 0x133b0e, 0x153e17, 0x1d421f, 0x1d401b, 0x1a421b, 0x234a1f, 0x2c5627, 0x2e562b, 0x224b28, 0x366531, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x17350f, 0x1b3d01, 0x365606, 0x466710, 0x476b1d, 0x21530c, 0x1f550f, 0x608242, 0x91b17e, 0xb4cda9, 0xa2cd84, 0x69c239, 0x75c823, 0x86ba58, 0x428a7c, 0x8b66, 0xa669, 0xc65e, 0xf6a3, 0xffbb, 0xdf94, 0x8c5b, 0x12754c, 0x7858, 0x624a, 0x4b4c, 0x11827f, 0xaecf9b, 0x9fc799, 0xa0c79b, 0x93bf8e, 0x6d9c66, 0x306c2b, 0x14108, 0x193d18, 0x224623, 0x264625, 0x284622, 0x2f4a27, 0x33502f, 0x365133, 0x345036, 0x4e7743, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x123c00, 0x96ae74, 0xdbebcc, 0xc7dcbc, 0xcde4c9, 0xb8d7b7, 0xc0ce9f, 0xa9b35f, 0xa0b972, 0x8cc969, 0xbe00, 0xbd00, 0x46c400, 0x3c9a49, 0x1b7791, 0x41b981, 0x2ec062, 0xa96d, 0xdd96, 0xdc7e, 0xc06b, 0x9069, 0x5b47, 0x6a53, 0x685c, 0x5556, 0x6173, 0x60b998, 0xaac587, 0x8cb886, 0x96bd8b, 0x83af7a, 0x518b4f, 0x226221, 0x1a4a1a, 0x224621, 0x274f24, 0x285122, 0x2d4d25, 0x354d30, 0x3f5338, 0x3f523d, 0x557c50, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x8aac6d, 0xffffff, 0xffffff, 0xffffff, 0xffffff, 0xffffff, 0xcfdeaa, 0x68b335, 0xb200, 0xad00, 0x6abe00, 0x63b600, 0x615f, 0x1d6d6f, 0x7ba7f, 0x42f281, 0xc666, 0x963c, 0x9931, 0x833d, 0x966f, 0x76c51, 0x402c, 0x5a3f, 0x4e4d, 0x6872, 0xb58b, 0x99bd68, 0x4e985b, 0x6daa6d, 0x8ab67b, 0x7eaa74, 0x64905c, 0x3d713a, 0x14521d, 0x195314, 0x225a13, 0x1e5319, 0x2f502e, 0x455a41, 0x4b5f4a, 0x587d52, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x205426, 0x3800, 0x4f8441, 0xbde4bb, 0xfcfffa, 0xffffff, 0xffffff, 0xcdd59e, 0xf9c00, 0xa500, 0x71a900, 0x96ae00, 0x5d8d35, 0x5765, 0x7247, 0x17c269, 0x29e77a, 0xa83f, 0x8a2d, 0x861e, 0x7636, 0x472d, 0xd4127, 0x7950, 0x673c, 0x5656, 0x676f, 0x3e8e4f, 0x9d9b1f, 0x688343, 0x639456, 0x71b56f, 0x91c082, 0x8ab37e, 0x6d9768, 0x357534, 0x1c5a00, 0x2b5400, 0x2c5311, 0x35522f, 0x4b5f4b, 0x576a59, 0x627d55, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x73a77b, 0x457947, 0x305e15, 0x58823b, 0x94c084, 0xc1e8bd, 0xf2f0d5, 0xcdd087, 0x449c00, 0x51a300, 0x7db800, 0x8da900, 0x617563, 0x1b8659, 0x8a40, 0x2a7230, 0x516b36, 0x218d58, 0xba57, 0x852f, 0x6345, 0x6135, 0x5228, 0x5a2f, 0x5031, 0x503d, 0x636a, 0x157245, 0x717d00, 0xa06e28, 0x8c6b2d, 0x569d4c, 0x73b86e, 0x87be7b, 0x83b078, 0x6a9359, 0x426c12, 0x3a5000, 0x3f4912, 0x404e35, 0x4d6653, 0x617265, 0x657a5b, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0xddf9e3, 0x96cb89, 0x6b9c47, 0x6c8d44, 0x5b8633, 0x478e2f, 0x86a357, 0xb19c48, 0x6a9000, 0x5ea500, 0x86cf00, 0x679236, 0x313b3c, 0x22b160, 0x5b22, 0x1b1413, 0x44734e, 0xc67a, 0xac65, 0x724a, 0x7e5a, 0x7a49, 0x754c, 0x764d, 0x472e, 0x3630, 0x6240, 0x9203, 0x8500, 0x625b00, 0x8b3600, 0x637821, 0x399a3d, 0x5aa054, 0x76a769, 0x7ba168, 0x668640, 0x605104, 0x613010, 0x584439, 0x526959, 0x64796a, 0x60745a, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0xb4dfaa, 0x7fb165, 0x6c9945, 0x61913f, 0x46831e, 0x327e09, 0x618423, 0xb97d35, 0x958e16, 0x74b000, 0x8bbf38, 0x5f6b3a, 0x294317, 0x2d6e39, 0x241e0f, 0x423b39, 0x905d, 0xb85d, 0x8559, 0x5141, 0x6937, 0x5623, 0x692c, 0x834e, 0x6247, 0x1e34, 0x205209, 0x55cb00, 0x9d0a, 0x2f7807, 0x705a00, 0x516b10, 0x37716, 0x247620, 0x47873f, 0x689759, 0x738f52, 0x83632d, 0x8b2912, 0x7a3029, 0x625b57, 0x596971, 0x636e65, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x619651, 0x507e46, 0x348136, 0x8209, 0x8300, 0x269234, 0x6faf64, 0xc09f63, 0xa39a43, 0x77c335, 0x80ad49, 0x6c4d38, 0x59631d, 0x2a4e0e, 0x1b0604, 0x1d5f50, 0xb979, 0xa04f, 0x9a6c, 0x9f9c, 0x5a62, 0x3386, 0x368b, 0x4052, 0x1c40, 0x29, 0x3b471a, 0x7dbe00, 0x5aa500, 0x11981a, 0x9111, 0x1a7b18, 0x276b23, 0xd631c, 0x6617, 0x3c752f, 0x7c8045, 0xa45e3c, 0xb83d31, 0xaa4b3f, 0x857360, 0x5f855c, 0x9cc79d, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0xc5e7c9, 0xb0b598, 0x987246, 0x6a761e, 0x428e2a, 0x61c16e, 0x7de79d, 0xa7b374, 0xa09054, 0x7ccc62, 0x709a41, 0x5f281e, 0x5d3c25, 0x163f07, 0x110c02, 0x9869, 0xab61, 0x712d, 0xb377, 0x866e, 0x68c1, 0x39cc, 0x13ab, 0x20c95, 0x30253, 0xd3d, 0x6e8f63, 0xb6b647, 0x6e9e00, 0x9d00, 0xa207, 0x8515, 0x1c6a2b, 0x386338, 0x54b18, 0x717742, 0xb7925d, 0xb6a048, 0xabb926, 0x8ad400, 0x73ed00, 0x7af500, 0x8fed00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0xffffff, 0xfff7f3, 0xf47175, 0xef4a54, 0xf06964, 0xf9a289, 0xeabe94, 0xe68b73, 0xde7d6f, 0x9dc788, 0x629055, 0x4d1f11, 0x28140e, 0x2917, 0x171707, 0x6333, 0x6d2e, 0x5308, 0x5f23, 0x451d, 0x5674, 0x2a77, 0x1054, 0x71054, 0x44, 0x4d4c, 0xbed079, 0xcfc26f, 0x3e8700, 0x7100, 0x8000, 0x911e, 0xb6c287, 0xcbc485, 0x8eaa76, 0xc7d838, 0xa2e600, 0x5df100, 0x14fb00, 0x59ed00, 0x90c700, 0x9da700, 0xd9b602, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0xffffff, 0xdff8e8, 0x678d5c, 0x6c7143, 0xa5715f, 0xc83941, 0xe03343, 0xeb4a5d, 0xed405d, 0xf26b7b, 0x84946c, 0x21290e, 0x90303, 0x1b0e09, 0x1b2008, 0x6830, 0x672b, 0x5f18, 0x3e0c, 0x3726, 0x1b20, 0x1750, 0x1339, 0x1, 0x1b3d, 0x7aa05b, 0xdcd46f, 0xa5ab4d, 0x488300, 0x7aa94f, 0x6cbc52, 0xbee3af, 0xffffff, 0xf3ffc8, 0xdbff14, 0x8bf800, 0xe700, 0xdc00, 0x6fd400, 0xb09e00, 0xb18f6b, 0xd9d3c2, 0xe1ebaa, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0xf8fffb, 0xb9debb, 0x589b52, 0x79935c, 0x8a815d, 0x7b624d, 0x767262, 0x7b8b7c, 0x867457, 0xb95d22, 0x847635, 0x1d2310, 0x70203, 0x230504, 0x114512, 0x7f35, 0x5c24, 0xd551c, 0x53312, 0x1919, 0x20e16, 0x9090f, 0x4040e, 0x101a19, 0x808e4c, 0xc6b550, 0x989c3c, 0x456800, 0xb3cc8b, 0xffffff, 0xffffff, 0xffffff, 0xfffff8, 0xc5fb4a, 0xbbd300, 0xd09a00, 0x80a300, 0x72aa00, 0x9fcd00, 0xcde8a6, 0xe2f8ed, 0xf4fff4, 0xc9ffe0, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0xc9f6d5, 0xa8b682, 0xbc774f, 0xb36e55, 0x817558, 0x67936e, 0x5eb567, 0x54c343, 0x2ccc00, 0x54c700, 0x41643f, 0x220509, 0x100403, 0x80302, 0x300a, 0x5b1a, 0x13710, 0x52a13, 0x1e11, 0x60e0c, 0x9090c, 0x20305, 0x80e05, 0x5f873f, 0xa0b351, 0x918327, 0x69852e, 0xc5d4b0, 0xfffafe, 0xe9ffef, 0xeaffdd, 0xd1f897, 0xd79924, 0xd59d0d, 0x9ab312, 0xdd8216, 0xff462f, 0xff6950, 0xc6fbcb, 0xdffff6, 0xc8f7cb, 0x95df97, 0xaae5b0, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x9dc290, 0x9ba86c, 0xa58d62, 0x5c7045, 0x3c733a, 0x9c00, 0xcb00, 0xc000, 0x26ba00, 0x8fc663, 0x334f4e, 0x70000, 0x381b14, 0x1b1208, 0x1e1c0c, 0x1c4518, 0xf3e1b, 0xb1213, 0x2070a, 0x1090c, 0x40b0d, 0x161107, 0x1c0e08, 0x716c4f, 0xb4a256, 0x875e02, 0xafaf87, 0xffffff, 0xecfff6, 0xcaffe0, 0xf5ffcb, 0xc39b52, 0xc52a26, 0xff365f, 0xcd8d5a, 0xdda83e, 0xe68b51, 0xaac596, 0x92ecaa, 0x86dc8b, 0x91e19a, 0xb5ebbc, 0xc2f2bd, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x7eab6b, 0x75c188, 0x86c092, 0x989d7a, 0xbbbc85, 0xc4d35c, 0xbec345, 0xd5e379, 0xededac, 0xffe2c9, 0xeac2a3, 0xe39871, 0xdd7b49, 0xbb5a24, 0xbe541a, 0x613726, 0x1a2822, 0x37200d, 0x321002, 0x431a03, 0x491b01, 0x833504, 0x481304, 0x42272f, 0xffc4a0, 0xffcfa2, 0xffe6c1, 0xfffff3, 0xe8ffd9, 0xecfff9, 0xdcfbdc, 0xe28457, 0xff763f, 0xcf9c00, 0xa4e500, 0xa4ce00, 0x93c57f, 0x5fd989, 0x61bf51, 0x7ed585, 0xade4ae, 0xcdfcc8, 0xc7f4b6, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0xe6ffe3, 0xf8f2d5, 0xffe5c6, 0xffffdd, 0xf7b9a1, 0xb4504c, 0xbd3834, 0xfa7f70, 0xf88d75, 0xd37134, 0xd78842, 0xd0883e, 0xa35608, 0x9c5000, 0x954c00, 0x48230d, 0x130609, 0x330e01, 0x4f1f01, 0x3e1101, 0x481401, 0x532102, 0x0, 0x210d10, 0xa97054, 0x977958, 0xa28443, 0xbf8745, 0xe98852, 0xf9ac79, 0xffcfaa, 0xffe7c5, 0xfff7c3, 0xffffc6, 0xfff580, 0xdae171, 0xabdca8, 0x5bc761, 0x55d375, 0xaef3c4, 0xd0f9cd, 0xc4fab2, 0x9dca84, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0xffc8bd, 0xe68364, 0xce6b2d, 0xbd6e3f, 0x6e190e, 0x60000, 0x360500, 0x5e2000, 0x6b3300, 0x260600, 0x90100, 0x241803, 0x191c03, 0x51601, 0x51603, 0x40402, 0x40301, 0x40402, 0x0, 0x321919, 0xfda277, 0xc77c52, 0x805539, 0x6c3f22, 0x7c1c05, 0x790000, 0x8f0000, 0x7b3813, 0x606937, 0x754200, 0x825000, 0x935716, 0xa25325, 0xc86e65, 0xfc937e, 0xffc094, 0xffeeb8, 0xfff3b4, 0xe0e9a6, 0xd5dfa8, 0xd6ffc6, 0xb0eba8, 0x619e5a, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x7e5700, 0x504400, 0x3b3600, 0x717620, 0xa2be7a, 0xaacea3, 0xa5d6a1, 0xb0aa3f, 0x665f2b, 0x2, 0x20002, 0x1b170f, 0x31107, 0x10a04, 0x10303, 0x10102, 0x30402, 0x40204, 0x0, 0x867721, 0xffde7e, 0xff9b66, 0xd87b4b, 0xc56c38, 0xe93f1b, 0xff0510, 0xff0005, 0xff3e30, 0x9bd493, 0x53cc7f, 0x649d00, 0x99600, 0x398500, 0x565400, 0x583c00, 0x784c1f, 0x9b5424, 0xb44d2a, 0xf17f5c, 0xff9a7d, 0xf8bfa1, 0xcdad79, 0xc0be8c, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x93ff00, 0x7ac700, 0x437b5a, 0x649c47, 0x83c14c, 0xb9ea86, 0xedffd5, 0xcadcae, 0xd0404, 0x0, 0x1b1308, 0x231c0f, 0x30402, 0x30302, 0x10101, 0x30302, 0xa0505, 0x90009, 0x334f00, 0x1aa500, 0x3d9300, 0x859a0c, 0xb37c1b, 0xd66b31, 0xde693c, 0xff4f37, 0xff0c17, 0xff0606, 0x91a700, 0x4acb24, 0x9db32e, 0x3fcc00, 0x67d200, 0x6ec30d, 0x4ba556, 0x2b9d47, 0x347f3d, 0x5b6733, 0x58410b, 0x570c00, 0x340000, 0x3b0000, 0xe57e61, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0xfeffe2, 0xd4ebaa, 0x819b74, 0x6e9266, 0x6a8864, 0x5a733e, 0x96ae4f, 0x5c7254, 0x0, 0x0, 0x281f10, 0x141109, 0x0, 0x20202, 0x50403, 0x130b0a, 0x17040e, 0x2d3f00, 0x9b000, 0xa200, 0x9c00, 0xb000, 0x52b800, 0x9d9b00, 0xae710c, 0xd2763a, 0xff744f, 0xff5938, 0x7fab00, 0x2cd900, 0x92ad00, 0x69b300, 0x63c600, 0x76b670, 0x7abf79, 0x47bd53, 0x77d482, 0xa4fca8, 0x99e990, 0x505e30, 0x38341a, 0x3a331e, 0x531801, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00, 0x2d00 };
	
static  void hubTestBmp(void) {
	copyScreen(screen, (screen_t *)testBmp);
}	

static int row, colorBit, colorI;

static enum hub_state {
	ST_PERIOD0 = 0,
	ST_PERIOD1,
	ST_PERIOD_DARK
} state;

static void initRows() {
	row = 0;
	colorBit = HUB_COLOR_BIT0;
	colorI = HUB_COLOR_I0;
	state = ST_PERIOD0;
}

int screenW, screenH;

static int screen_dx, screen_dy;
static int screen0;

static orient_t currentOrient = HUB_ROTATE_NOTSET;

void hubSetOrient(orient_t orient) {
	hubOrientation = orient;
	switch (hubOrientation) {
		case HUB_ROTATE_90:
			screenW = SCREEN_H;
			screenH = SCREEN_W;
			break;

		case HUB_ROTATE_180:
			screenW = SCREEN_W;
			screenH = SCREEN_H;
			break;

		case HUB_ROTATE_270:
			screenW = SCREEN_H;
			screenH = SCREEN_W;
			break;

		case HUB_ROTATE_0:
		default:
			screenW = SCREEN_W;
			screenH = SCREEN_H;
			break;
	}
}

static inline void setOrientSwap() {
	if (hubOrientation != currentOrient) {
		currentOrient = hubOrientation;
		switch (currentOrient) {
			case HUB_ROTATE_90:
				screen_dx = -screenW;
				screen_dy = 1;
				screen0 = screenW * (screenH - 1);
				break;

			case HUB_ROTATE_180:
				screen_dx = -1;
				screen_dy = -screenW;
				screen0 = SCREEN_LEN - 1;
				break;

			case HUB_ROTATE_270:
				screen_dx = screenW;
				screen_dy = -1;
				screen0 = screenW - 1;
				break;

			case HUB_ROTATE_0:
			default:
				screen_dx = 1;
				screen_dy = screenW;
				screen0 = 0;
				break;
		}
	}
}

void hubInit(void) {
	initIO();
	calculatePeriod();
	hubLUTInit();
	initRows();
	hubOrientation = HUB_ROTATE_0;
	hubSetOrient(hubOrientation);
	clearScreen();
	//fillScreen(COLOR_RGB(255, 255, 255));
	hubTestBmp();
	screenRedraw();
	
	// 100 Hz * 16 Phases
  HUB_TIMER_CLK_ENABLE();
  TIM_ClockConfigTypeDef sClockSourceConfig;

  hubtim.Instance = HUB_TIMER;
  hubtim.Init.Prescaler = HUB_PRESCALER - 1;
  hubtim.Init.CounterMode = TIM_COUNTERMODE_UP;
  hubtim.Init.Period = period0 - 1;
  hubtim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  HAL_TIM_Base_Init(&hubtim);

  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  HAL_TIM_ConfigClockSource(&hubtim, &sClockSourceConfig);

	HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
  HAL_NVIC_SetPriority(TIM1_BRK_TIM9_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);
	
	HAL_TIM_Base_Start_IT(&hubtim);
	
	// Timer 2 for NOE
	HUB_TIMER2_CLK_ENABLE();

  TIM_OC_InitTypeDef sConfigOC;

  hubtim2.Instance = HUB_TIMER2;
  hubtim2.Init.Prescaler = 0;
  hubtim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  hubtim2.Init.Period = 0xffff;
  hubtim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  HAL_TIM_Base_Init(&hubtim2);

  HAL_TIM_OC_Init(&hubtim2);

  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = period2Min;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_LOW;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  HAL_TIM_OC_ConfigChannel(&hubtim2, &sConfigOC, NOE_TIM_CHANNEL);

	HAL_TIM_PWM_Start(&hubtim2, NOE_TIM_CHANNEL); 

}

static inline void outputRow(void) {
	pinWrite(hub_a, row & 1);
	pinWrite(hub_b, row & 2);
	pinWrite(hub_c, row & 4);
	pinWrite(hub_d, row & 8);
}

static 	color_t * buf, * buf2, * bufLine, * bufLine2;
static int period1n;

static inline void hubTask() {
	color_t pixel, pixel2;

	if (state == ST_PERIOD0) {
		//Period 0 - Shift row data
		pinReset(hub_time);
		//__HAL_TIM_SetAutoreload(&hubtim, period0 - 1);
		HUB_TIMER->ARR = period0 - 1;
		
		if (colorBit == HUB_COLOR_BIT0) {
			if (row == 0) {
				//Begin of Frame
				setBrightness();
				if (hubBrightness == 0) {
					//Display turn off
					//__HAL_TIM_SetAutoreload(&hubtim, FRAME_PERIOD - 1);
					HUB_TIMER->ARR = FRAME_PERIOD - 1;

					pinSet(hub_time);
					return;
				}
				if (hubNeedRedraw) {
					swapPages();
					setOrientSwap();
				}	
				bufLine = &DISPLAY[screen0];
				bufLine2 = &bufLine[HUB_ROWS * screen_dy];
			}
			outputRow();
		}
	
		int red, green, blue;
		int red2, green2, blue2;
		buf = bufLine;
		buf2 = bufLine2;
		for (int x = 0; x < SCREEN_W; x++) {
			pixel = *buf;
			red = hubLUT[pixel.R];
			green = hubLUT[pixel.G];
			blue = hubLUT[pixel.B];

			pixel2 = *buf2;
			red2 = hubLUT[pixel2.R];
			green2 = hubLUT[pixel2.G];
			blue2 = hubLUT[pixel2.B];

			pinReset(hub_clk);
			
			pinWrite(hub_r1, red & colorBit);
			pinWrite(hub_g1, green & colorBit);
			pinWrite(hub_b1, blue & colorBit);
			
			pinWrite(hub_r2, red2 & colorBit);
			pinWrite(hub_g2, green2 & colorBit);
			pinWrite(hub_b2, blue2 & colorBit);
			
			buf += screen_dx;
			buf2 += screen_dx;
		
			pinSet(hub_clk);
		}
		period1n = period1N[colorI];
		prescaler2n = prescaler2N[colorI];
		colorBit >>= 1;
		colorI--;
		if (colorBit == 0) {
			colorBit = HUB_COLOR_BIT0;
			colorI = HUB_COLOR_I0;
			//Change Row
			row = (row + 1) & HUB_ROWS_MASK;
			bufLine += screen_dy;
			bufLine2 += screen_dy;
		}	
		pinSet(hub_lat);
		state = ST_PERIOD1;
		pinReset(hub_lat);
	} else if (state == ST_PERIOD1) {
		//Period 1 - Show current row
		
		//Set prescaler
		HUB_TIMER2->PSC = prescaler2n - 1;
		//Update Event Generation
		HUB_TIMER2->EGR = TIM_EGR_UG;
		//Enable Timer
		HUB_TIMER2->CR1 |= TIM_CR1_CEN;
		//Set Period
		//__HAL_TIM_SetAutoreload(&hubtim, period1n - 1);
		HUB_TIMER->ARR = period1n - 1;
		state = ST_PERIOD0;
		if ((colorBit == HUB_COLOR_BIT0) && (row == 0)) {
			//Last pixels
			state = ST_PERIOD_DARK;
		}
	}	else if (state == ST_PERIOD_DARK) {
		//Period Dark - End of frame
		//Set period
		HUB_TIMER->ARR = periodDark - 1;
		//Disable Timer
		HUB_TIMER2->CR1 &= ~TIM_CR1_CEN;

		state = ST_PERIOD0;
	}

	pinSet(hub_time);
}

void TIM1_BRK_TIM9_IRQHandler(void)
{
	//if (__HAL_TIM_GET_ITSTATUS(&hubtim, TIM_IT_UPDATE) != RESET) {
	if ((HUB_TIMER->DIER & TIM_IT_UPDATE) != 0) {	
		hubTask();
		//__HAL_TIM_CLEAR_IT(&hubtim, TIM_IT_UPDATE);
		(HUB_TIMER->SR = ~(TIM_IT_UPDATE));
	}
	//HAL_NVIC_ClearPendingIRQ(TIM1_BRK_TIM9_IRQn);
	NVIC->ICPR[TIM1_BRK_TIM9_IRQn >> 5] = 1 << TIM1_BRK_TIM9_IRQn;
}

void inline clearScreen(void) {
	memset(screen, 0, SCREEN_BYTES);
}

void inline copyScreen(screen_t * dst, screen_t * src) {
	memmove(dst, src, SCREEN_BYTES);
}

void fillScreen(color_t color) {
	color_t * sc = (color_t *)screen;
	for (int i = 0; i < SCREEN_LEN; i++) {
		*sc = color;
		sc++;
	}
}
