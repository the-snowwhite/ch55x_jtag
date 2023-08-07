/********************************** (C) COPYRIGHT ************* *********************
 * File Name 		: CDC.C
 * Author 			: Kongou Hikari
 * Version 			: V1.0
 * Date 			: 2019/02/16
 * Description 		: CH552 USB to JTAG with FTDI Protocol
 ***************************************************** *******************************/
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ch554.h>
#include <ch554_usb.h>
#include <debug.h>

/*
 * Use T0 to count the SOF_Count every 1ms
 * If you doesn't like this feature, define SOF_NO_TIMER
 * Background: The usb host must to send SOF every 1ms, but some USB host don't really do that
 * FTDI's driver has some bug, if it doesn't received empty packet with modem status,
 * it will causes BSoD, so highly recommended use T0 instead of SOF packet to generate empty packet report.
 */
//#define SOF_NO_TIMER

/*
 * M *emory map:
 * EP0 Buf 	00 - 3f
 * EP4 Buf 	40 - 7f
 * EP1 Buf 	80 - bf
 * RingBuf 	100 - 1ff
 * EP2 Buf 	300 - 37f
 * EP3 Buf 	380 - 3bf
 */

__xdata __at (0x0000) uint8_t Ep0Buffer[DEFAULT_ENDP0_SIZE]; 	// endpoint 0 OUT&IN buffer, must be an even address

__xdata __at (0x0080) uint8_t Ep1Buffer[MAX_PACKET_SIZE]; 		// Endpoint 1 IN send buffer
__xdata __at (0x0300) uint8_t Ep2Buffer[MAX_PACKET_SIZE * 2]; 	// Endpoint 2 OUT receive buffer

__xdata __at (0x0380) uint8_t Ep3Buffer[MAX_PACKET_SIZE]; 		// endpoint 3 IN send buffer
__xdata __at (0x0040) uint8_t Ep4Buffer[MAX_PACKET_SIZE]; 	// Endpoint 4 OUT receive buffer

__xdata __at (0x0100) uint8_t RingBuf[128];

uint16_t SetupLen;
uint8_t SetupReq, Count, UsbConfig;
uint8_t VendorControl;

__code uint8_t * pDescr; 													//USB configuration flag
uint8_t pDescr_Index = 0;
USB_SETUP_REQ SetupReqBuf; 												// Temporarily save the Setup package
#define UsbSetupBuf 	((PUSB_SETUP_REQ)Ep0Buffer)

#define SBAUD_TH 		104U 	// 16M/16/9600
#define SBAUD_SET 		128000U 	// Baud rate of serial port 0

/*Device descriptor*/
__code uint8_t DevDesc[] = {0x12, 0x01, 0x00, 0x02,
    0x00, 0x00, 0x00, DEFAULT_ENDP0_SIZE,
    0x03, 0x04, 0x10, 0x60, 0x00, 0x05, 0x01, 0x02,
    0x03, 0x01
};
__code uint16_t itdf_eeprom[] =
{
    0x0800, 0x0403, 0x6010, 0x0500, 0x3280, 0x0000, 0x0200, 0x1096,
    0x1aa6, 0x0000, 0x0046, 0x0310, 0x004f, 0x0070, 0x0065, 0x006e,
    0x002d, 0x0045, 0x0043, 0x031a, 0x0055, 0x0053, 0x0042, 0x0020,
    0x0044, 0x0065, 0x0062, 0x0075, 0x0067, 0x0067, 0x0065, 0x0072,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1027
};
__code uint8_t CfgDesc[] =
{
    0x09, 0x02, sizeof(CfgDesc) & 0xff, sizeof(CfgDesc) >> 8,
    0x02, 0x01, 0x00, 0x80, 0x32, 		// configuration descriptor (1 interface)
    //The following is the interface 0 (data interface) descriptor
    0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0xff, 0xff, 0x04, 	// Data interface descriptor
    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00, 				// endpoint descriptor EP1 BULK IN
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00, 				// endpoint descriptor EP2 BULK OUT
    //The following is the interface 1 (data interface) descriptor
    0x09, 0x04, 0x01, 0x00, 0x02, 0xff, 0xff, 0xff, 0x00, 	// Data interface descriptor
    0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00, 				// endpoint descriptor EP3 BULK IN
    0x07, 0x05, 0x04, 0x02, 0x40, 0x00, 0x00, 				// endpoint descriptor EP4 BULK OUT
};
/*String descriptor*/
unsigned char __code LangDes[] = {0x04, 0x03, 0x09, 0x04}; 	// language descriptor

unsigned char __code Prod_Des[] = 								// product string descriptor
{
    sizeof(Prod_Des), 0x03,
    'S', 0x00, 'i', 0x00, 'p', 0x00, 'e', 0x00, 'e', 0x00, 'd', 0x00,
    '-', 0x00, 'D', 0x00, 'e', 0x00, 'b', 0x00, 'u', 0x00, 'g', 0x00
};
unsigned char __code Jtag_Des[] = 								// product string descriptor
{
    sizeof(Jtag_Des), 0x03,
    'S', 0x00, 'i', 0x00, 'p', 0x00, 'e', 0x00, 'e', 0x00, 'd', 0x00,
    '-', 0x00, 'J', 0x00, 'T', 0x00, 'A', 0x00, 'G', 0x00
};
unsigned char __code Manuf_Des[] =
{
    sizeof(Manuf_Des), 0x03,
    'K', 0x00, 'o', 0x00, 'n', 0x00, 'g', 0x00, 'o', 0x00, 'u', 0x00,
    ' ', 0x00, 'H', 0x00, 'i', 0x00, 'k', 0x00, 'a', 0x00, 'r', 0x00, 'i', 0x00
};

__code uint8_t QualifierDesc[]=
{
    10, 	/* bLength */
    USB_DESCR_TYP_QUALIF, 	/* bDescriptorType */

    0x00, 0x02, 		/*bcdUSB */

    0xff, /* bDeviceClass */
    0xff, /* bDeviceSubClass */
    0xff, /* bDeviceProtocol */

    DEFAULT_ENDP0_SIZE, /* bMaxPacketSize0 */
    0x00, /* bNumOtherSpeedConfigurations */
    0x00 /* bReserved */
};

/* download control */
volatile __idata uint8_t USBOutLength = 0;
volatile __idata uint8_t USBOutPtr = 0;
volatile __idata uint8_t USBReceived = 0;

volatile __idata uint8_t Serial_Done = 0;
volatile __idata uint8_t USB_Require_Data = 0;

volatile __idata uint8_t USBOutLength_1 = 0;
volatile __idata uint8_t USBOutPtr_1 = 0;
volatile __idata uint8_t USBReceived_1 = 0;
/* upload control */
volatile __idata uint8_t UpPoint1_Busy = 0; // Whether the upload endpoint is busy or not
volatile __idata uint8_t UpPoint1_Ptr = 2;

volatile __idata uint8_t UpPoint3_Busy = 0; // Whether the upload endpoint is busy
volatile __idata uint8_t UpPoint3_Ptr = 2;

/* Miscellaneous */
volatile __idata uint16_t SOF_Count = 0;
volatile __idata uint8_t Latency_Timer = 4; //Latency Timer
volatile __idata uint8_t Latency_Timer1 = 4;
volatile __idata uint8_t Require_DFU = 0;

/* Flow Control*/
volatile __idata uint8_t soft_dtr = 0;
volatile __idata uint8_t soft_rts = 0;

/* MPSSE settings */

volatile __idata uint8_t Mpsse_Status = 0;
volatile __idata uint16_t Mpsse_LongLen = 0;
volatile __idata uint8_t Mpsse_ShortLen = 0;

#define HARD_ESP_CTRL 1

#ifndef HARD_ESP_CTRL
volatile __idata uint8_t Esp_Boot_Chk = 0;
volatile __idata uint8_t Esp_Require_Reset = 0;
#endif

/**************************************************** *******************************
 * Function Name: USBDeviceCfg()
 * Description 	: USB device mode configuration
 * Input 		: None
 * Output 		: None
 * Return 		: None
 ***************************************************** *******************************/
void USBDeviceCfg()
{
    USB_CTRL = 0x00; 														// clear USB control register
    USB_CTRL &= ~bUC_HOST_MODE; 												// This bit is to select the device mode
    USB_CTRL |= bUC_DEV_PU_EN | bUC_INT_BUSY | bUC_DMA_EN; 					//USB device and internal pull-up enable, automatically return NAK before the interrupt flag is cleared during the interrupt
    USB_DEV_AD = 0x00; 														// device address initialization
    // 	USB_CTRL |= bUC_LOW_SPEED;
    // 	UDEV_CTRL |= bUD_LOW_SPEED; 												// Select low speed 1.5M mode
    USB_CTRL &= ~bUC_LOW_SPEED;
    UDEV_CTRL &= ~bUD_LOW_SPEED; 											// Select full speed 12M mode, the default mode
    UDEV_CTRL = bUD_PD_DIS; // disable DP/DM pull-down resistor
    UDEV_CTRL |= bUD_PORT_EN; 												// Enable the physical port
}

void Jump_to_BL()
{
    ES = 0;
    PS = 0;

    P1_DIR_PU = 0;
    P1_MOD_OC = 0;
    P1 = 0xff;

    USB_INT_EN = 0;
    USB_CTRL = 0x06;
    //UDEV_CTRL = 0x80;

    mDelaymS(100);

    EA = 0;

    while(1)
    {
        __asm
        LJMP 0x3800
        __endasm;
    }
}
/**************************************************** *******************************
 * Function Name : USBDeviceIntCfg()
 * Description 	: USB device mode interrupt initialization
 * Input 		: None
 * Output 		: None
 * Return 		: None
 ***************************************************** *******************************/
void USBDeviceIntCfg()
{
    USB_INT_EN |= bUIE_SUSPEND; 											// Enable device suspend interrupt
    USB_INT_EN |= bUIE_TRANSFER; 											// Enable USB transfer complete interrupt
    USB_INT_EN |= bUIE_BUS_RST; 											// Enable device mode USB bus reset interrupt
    USB_INT_EN |= bUIE_DEV_SOF; 													// Enable SOF interrupt
    USB_INT_FG |= 0x1F; 													// clear interrupt flag
    IE_USB = 1; 															// Enable USB interrupt
    EA = 1; 																// Enable MCU interrupt
}
/**************************************************** *******************************
 * Function Name : USBDeviceEndPointCfg()
 * Description 	: USB device mode endpoint configuration, simulating a compatible HID device, in addition to the control transmission of endpoint 0, it also includes batch upload and download of endpoint 2
 * Input 		: None
 * Output 		: None
 * Return 		: None
 ***************************************************** *******************************/
void USBDeviceEndPointCfg()
{
    // TODO: Is casting the right thing here? What about endianness?
    UEP2_DMA = (uint16_t) Ep2Buffer; 											// Endpoint 2 OUT receives data transmission address
    UEP3_DMA = (uint16_t) Ep3Buffer;
    UEP2_3_MOD = 0x48; 															// endpoint 2 single buffer receive, endpoint 3 single buffer send
    //UEP2_3_MOD = 0x49; 				// Endpoint 3 single-buffered transmission, endpoint 2 double-buffered reception

    UEP2_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK; 									// Endpoint 2 automatically flips the synchronization flag, OUT returns ACK
    UEP3_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK; //Endpoint 3 sends and returns NAK

    //UEP4_DMA = (uint16_t) Ep4Buffer; //Ep4Buffer = Ep0Buffer + 64
    UEP1_DMA = (uint16_t) Ep1Buffer; 										// endpoint 1 IN send data transmission address
    UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK; 								// Endpoint 1 automatically flips the synchronization flag, IN transaction returns NAK
    UEP4_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK; //Endpoint 4 receives and returns ACK, which cannot be automatically reversed
    UEP4_1_MOD = 0x48; 														// Endpoint 1 single buffer send, endpoint 4 single buffer receive

    UEP0_DMA = (uint16_t) Ep0Buffer; 													// Endpoint 0 data transfer address
    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK; 								// Manual flip, OUT transaction returns ACK, IN transaction returns NAK
}

__code uint8_t HexToAscTab[] = "0123456789ABCDEF";

void uuidcpy(__xdata uint8_t *dest, uint8_t index, uint8_t len) /* Use UUID to generate USB Serial Number */
{
    uint8_t i;
    uint8_t p = 0; /* UUID format, ten hexadecimal numbers*/
    __code uint8_t *puuid;
    for(i = index; i < (index + len); i++)
    {
        if(i == 0)
            dest[p++] = 22; //10 * 2 + 2
            else if(i == 1)
                dest[p++] = 0x03;
        else
        {
            if(i & 0x01) //odd number
            {
                dest[p++] = 0x00;
            }
            else
            {
                puuid = (__code uint8_t *) (0x3ffa + (i - 2) / 4);
                if(i & 0x02)
                    dest[p++] = HexToAscTab[(*puuid) >> 4];
                else
                    dest[p++] = HexToAscTab[(*puuid) & 0x0f];
            }
        }
    }
}

#define INTF1_DTR 	TIN1
#define INTF1_RTS 	TIN0

#define INTF2_DTR 	TIN3
#define INTF2_RTS 	TIN2

volatile __idata uint8_t DTR_State = 0;
volatile __idata uint8_t Modem_Count = 0;

/**************************************************** *******************************
 * Function Name : DeviceInterrupt()
 * Description 	: CH559USB interrupt processing function
 ***************************************************** *******************************/
void DeviceInterrupt(void) __interrupt (INT_NO_USB) 					//USB interrupt service routine, use register group 1
{
    uint16_t len;
    uint16_t divisor;
    if ((USB_INT_ST & MASK_UIS_TOKEN) == UIS_TOKEN_SOF)
    {
        #ifdef SOF_NO_TIMER
        SOF_Count ++;
        if(Modem_Count)
            Modem_Count --;
        if(Modem_Count == 1)
        {
            if(soft_dtr == 0 && soft_rts == 1)
            {
                INTF1_RTS = 1;
                INTF1_DTR = 0;
            }
            if(soft_dtr == 1 && soft_rts == 0)
            {
                INTF1_RTS = 0;
                INTF1_DTR = 1;
            }
            if(soft_dtr == soft_rts)
            {
                INTF1_DTR = 1;
                INTF1_RTS = 0;
                INTF1_RTS = 1;
            }
        }
        if(SOF_Count % 16 == 0)
            PWM2 = 1;
        #endif
    }
    if(UIF_TRANSFER) 															//USB transfer complete flag
    {
        switch (USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP))
        {
            case UIS_TOKEN_IN | 1: 												//endpoint 1# Endpoint bulk upload
                UEP1_T_LEN = 0;
                UEP1_CTRL = UEP1_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_NAK; 		// Default answer NAK
                UpPoint1_Busy = 0; 												// clear busy flag
                break;
            case UIS_TOKEN_OUT | 2: 												//endpoint 2# Endpoint bulk download
                if ( U_TOG_OK ) 													// Out of sync packets will be discarded
                {
                    UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_NAK; 	// When a packet of data is received, it will be NAK. After the main function finishes processing, the main function will modify the response mode
                    USBOutLength = USB_RX_LEN;
                    USBOutPtr = 0;
                    USBReceived = 1;
                }
                break;
            case UIS_TOKEN_IN | 3: 												//endpoint 3# Endpoint bulk upload
                UEP3_T_LEN = 0;
                UEP3_CTRL = UEP3_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_NAK; 		// Default answer NAK
                UpPoint3_Busy = 0; 												// clear busy flag
                break;
            case UIS_TOKEN_OUT | 4: 												//endpoint 4# Endpoint batch download
                if ( U_TOG_OK ) 													// Out of sync packets will be discarded
                {
                    UEP4_CTRL ^= bUEP_R_TOG; 	// Synchronization flag flipped
                    UEP4_CTRL = UEP4_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_NAK; 	// NAK when a packet of data is received, the main function finishes processing, and the main function modifies the response mode
                    USBOutLength_1 = USB_RX_LEN + 64;
                    USBOutPtr_1 = 64;
                    USBReceived_1 = 1;
                }
                break;
            case UIS_TOKEN_SETUP | 0: 												//SETUP transaction
                len = USB_RX_LEN;
                if(len == (sizeof(USB_SETUP_REQ)))
                {
                    SetupLen = ((uint16_t)UsbSetupBuf->wLengthH << 8) | (UsbSetupBuf->wLengthL);
                    len = 0; 													// The default is success and the upload length is 0
                    VendorControl = 0;
                    SetupReq = UsbSetupBuf->bRequest;
                    if ( ( UsbSetupBuf->bRequestType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )//non-standard request
                    {
                        //TODO: Rewrite
                        VendorControl = 1;
                        if(UsbSetupBuf->bRequestType & USB_REQ_TYP_READ)
                        {
                            //read
                            switch( SetupReq )
                            {
                                case 0x90: //READ EEPROM
                                    divisor = UsbSetupBuf->wIndexL & 0x3f;
                                    Ep0Buffer[0] = itdf_eeprom[divisor] & 0xff;
                                    Ep0Buffer[1] = itdf_eeprom[divisor] >> 8;
                                    len = 2;
                                    break;
                                case 0x0a:
                                    if(UsbSetupBuf->wIndexL == 2)
                                        Ep0Buffer[0] = Latency_Timer1;
                                else
                                    Ep0Buffer[0] = Latency_Timer;
                                len = 1;
                                break;
                                case 0x05:
                                    Ep0Buffer[0] = 0x01;
                                    Ep0Buffer[1] = 0x60;
                                    len = 2;
                                    break;
                                default:
                                    len = 0xFF; 	/* command not supported*/
                                    break;
                            }
                        }
                        else
                        {
                            //Write
                            switch( SetupReq )
                            {
                                case 0x02:
                                case 0x04:
                                case 0x06:
                                case 0x07:
                                case 0x0b:
                                case 0x92:
                                    len = 0;
                                    break;
                                case 0x91: //WRITE EEPROM, FT_PROG action, directly jump to BL
                                    Require_DFU = 1;
                                    len = 0;
                                    break;
                                case 0x00:
                                    if(UsbSetupBuf->wIndexL == 1)
                                        UpPoint1_Busy = 0;
                                if(UsbSetupBuf->wIndexL == 2)
                                {
                                    UpPoint3_Busy = 0;
                                    UEP4_CTRL &= ~(bUEP_R_TOG);
                                }
                                len = 0;
                                break;
                                case 0x09: //SET LATENCY TIMER
                                    if(UsbSetupBuf->wIndexL == 1)
                                        Latency_Timer = UsbSetupBuf->wValueL;
                                else
                                    Latency_Timer1 = UsbSetupBuf->wValueL;
                                len = 0;
                                break;
                                case 0x03:
                                    //divisor = wValue
                                    //U1SMOD = 1;
                                    //PCON |= SMOD; //double the baud rate
                                    //T2MOD |= bTMR_CLK; //The highest count clock
                                    PCON |= SMOD;
                                    T2MOD |= bT1_CLK;

                                    divisor = UsbSetupBuf->wValueL |
                                    (UsbSetupBuf->wValueH << 8);
                                    divisor &= 0x3fff; //Can't generate decimal and integer part, baudrate = 48M/16/divisor

                                    if(divisor == 0 || divisor == 1) //baudrate > 3M
                                    {
                                        if(UsbSetupBuf->wIndexL == 2)
                                            TH1 = 0xff; //I really can't hold back 1M
                                    }
                                    else
                                    {
                                        uint16_t div_tmp = 0;
                                        div_tmp = 10 * divisor / 3; //16M CPU clock
                                        if (div_tmp % 10 >= 5) 	divisor = div_tmp / 10 + 1;
                                        else 					divisor = div_tmp / 10;

                                        if(divisor > 256)
                                        {
                                            //TH1 = 0 - SBAUD_TH; //All use the preset baud rate
                                            if(UsbSetupBuf->wIndexL == 2)
                                            {
                                                divisor /= 12;
                                                if(divisor > 256) //set baud rate less than 488
                                                {
                                                    TH1 = 0 - SBAUD_TH; //9600bps
                                                }
                                                else
                                                {
                                                    //PCON &= ~(SMOD);
                                                    T2MOD &= ~(bT1_CLK); //low baud rate
                                                    TH1 = 0 - divisor;
                                                }
                                            }
                                        }
                                        else
                                        {
                                            if(UsbSetupBuf->wIndexL == 2)
                                                TH1 = 0 - divisor;
                                            #if 0
                                            else //intf2
                                                SBAUD1 = 0 - divisor;
                                            #endif
                                        }
                                    }
                                    len = 0;
                                    break;
                                    case 0x01: //MODEM Control
                                        #if HARD_ESP_CTRL
                                        if(UsbSetupBuf->wIndexL == 2)
                                        {
                                            if(UsbSetupBuf->wValueH & 0x01)
                                            {
                                                if(UsbSetupBuf->wValueL & 0x01) //DTR
                                                {
                                                    soft_dtr = 1;
                                                    //INTF1_DTR = 0;
                                                }
                                                else
                                                {
                                                    soft_dtr = 0;
                                                    //INTF1_DTR = 1;
                                                }
                                            }
                                            if(UsbSetupBuf->wValueH & 0x02)
                                            {
                                                if(UsbSetupBuf->wValueL & 0x02) //RTS
                                                {
                                                    soft_rts = 1;
                                                    //INTF1_RTS = 0;
                                                }
                                                else
                                                {
                                                    soft_rts = 0;
                                                    //INTF1_RTS = 1;
                                                }
                                            }
                                            Modem_Count = 20;
                                        }
                                        #else
                                        if(Esp_Require_Reset == 3)
                                        {
                                            CAP1 = 0;
                                            Esp_Require_Reset = 4;
                                        }
                                        #endif
                                        len = 0;
                                        break;
                                    default:
                                        len = 0xFF; 		/* command not supported*/
                                        break;
                            }
                        }

                    }
                    else 															// Standard request
                    {
                        switch(SetupReq) 											// request code
                        {
                            case USB_GET_DESCRIPTOR:
                                switch(UsbSetupBuf->wValueH)
                                {
                                    case USB_DESCR_TYP_DEVICE: 													// device descriptor
                                        pDescr = DevDesc; 										// Send the device descriptor to the buffer to be sent
                                        len = sizeof(DevDesc);
                                        break;
                                    case USB_DESCR_TYP_CONFIG: 														// configuration descriptor
                                        pDescr = CfgDesc; 										// Send the device descriptor to the buffer to be sent
                                        len = sizeof(CfgDesc);
                                        break;
                                    case USB_DESCR_TYP_STRING:
                                        if(UsbSetupBuf->wValueL == 0)
                                        {
                                            pDescr = LangDes;
                                            len = sizeof(LangDes);
                                        }
                                        else if(UsbSetupBuf->wValueL == 1)
                                        {
                                            pDescr = Manuf_Des;
                                            len = sizeof(Manuf_Des);
                                        }
                                        else if(UsbSetupBuf->wValueL == 2)
                                        {
                                            pDescr = Prod_Des;
                                            len = sizeof(Prod_Des);
                                        }
                                        else if(UsbSetupBuf->wValueL == 4)
                                        {
                                            pDescr = Jtag_Des;
                                            len = sizeof(Jtag_Des);
                                        }
                                        else
                                        {
                                            pDescr = (__code uint8_t *)0xffff;
                                            len = 22; /* 10-digit ASCII serial number */
                                        }
                                        break;
                                    case USB_DESCR_TYP_QUALIF:
                                        //pDescr = QualifierDesc;
                                        //len = sizeof(QualifierDesc);
                                        len = 0xff;
                                        break;
                                    default:
                                        len = 0xff; 												// unsupported command or error
                                        break;
                                }

                                if ( SetupLen > len )
                                {
                                    SetupLen = len; 	// Limit the total length
                                }
                                if (len != 0xff)
                                {
                                    len = SetupLen >= DEFAULT_ENDP0_SIZE ? DEFAULT_ENDP0_SIZE : SetupLen; 							// Length of this transmission

                                    if(pDescr == (__code uint8_t *) 0xffff) /* Take the serial number*/
                                    {
                                        uuidcpy(Ep0Buffer, 0, len);
                                    }
                                    else
                                    {
                                        memcpy(Ep0Buffer, pDescr, len); 								// load upload data
                                    }
                                    SetupLen -= len;
                                    pDescr_Index = len;
                                }
                                break;
                                    case USB_SET_ADDRESS:
                                        SetupLen = UsbSetupBuf->wValueL; 							// Store USB device address temporarily
                                        break;
                                    case USB_GET_CONFIGURATION:
                                        Ep0Buffer[0] = UsbConfig;
                                        if ( SetupLen >= 1 )
                                        {
                                            len = 1;
                                        }
                                        break;
                                    case USB_SET_CONFIGURATION:
                                        UsbConfig = UsbSetupBuf->wValueL;
                                        break;
                                    case USB_GET_INTERFACE:
                                        break;
                                    case USB_CLEAR_FEATURE: 											//Clear Feature
                                        if( ( UsbSetupBuf->bRequestType & 0x1F ) == USB_REQ_RECIP_DEVICE ) 				/* clear device*/
                                        {
                                            if( ( ( ( uint16_t )UsbSetupBuf->wValueH << 8 ) | UsbSetupBuf->wValueL ) == 0x01 )
                                            {
                                                if( CfgDesc[ 7 ] & 0x20 )
                                                {
                                                    /* wake up */
                                                }
                                                else
                                                {
                                                    len = 0xFF; 										/* operation failed*/
                                                }
                                            }
                                            else
                                            {
                                                len = 0xFF; 											/* operation failed*/
                                            }
                                        }
                                        else if ( ( UsbSetupBuf->bRequestType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )// endpoint
                                        {
                                            switch( UsbSetupBuf->wIndexL )
                                            {
                                                case 0x83:
                                                    UEP3_CTRL = UEP3_CTRL & ~ ( bUEP_T_TOG | MASK_UEP_T_RES ) | UEP_T_RES_NAK;
                                                    break;
                                                case 0x03:
                                                    UEP3_CTRL = UEP3_CTRL & ~ ( bUEP_R_TOG | MASK_UEP_R_RES ) | UEP_R_RES_ACK;
                                                    break;
                                                case 0x82:
                                                    UEP2_CTRL = UEP2_CTRL & ~ ( bUEP_T_TOG | MASK_UEP_T_RES ) | UEP_T_RES_NAK;
                                                    break;
                                                case 0x02:
                                                    UEP2_CTRL = UEP2_CTRL & ~ ( bUEP_R_TOG | MASK_UEP_R_RES ) | UEP_R_RES_ACK;
                                                    break;
                                                case 0x81:
                                                    UEP1_CTRL = UEP1_CTRL & ~ ( bUEP_T_TOG | MASK_UEP_T_RES ) | UEP_T_RES_NAK;
                                                    break;
                                                case 0x01:
                                                    UEP1_CTRL = UEP1_CTRL & ~ ( bUEP_R_TOG | MASK_UEP_R_RES ) | UEP_R_RES_ACK;
                                                    break;
                                                default:
                                                    len = 0xFF; 										// unsupported endpoint
                                                    break;
                                            }
                                            UpPoint1_Busy = 0;
                                            UpPoint3_Busy = 0;
                                        }
                                        else
                                        {
                                            len = 0xFF; 												// It is not an endpoint that does not support
                                        }
                                        break;
                                                case USB_SET_FEATURE: 										/* Set Feature */
                                                    if( ( UsbSetupBuf->bRequestType & 0x1F ) == USB_REQ_RECIP_DEVICE ) 				/* setup device*/
                                                    {
                                                        if( ( ( ( uint16_t )UsbSetupBuf->wValueH << 8 ) | UsbSetupBuf->wValueL ) == 0x01 )
                                                        {
                                                            if( CfgDesc[ 7 ] & 0x20 )
                                                            {
                                                                /* sleep */
                                                                #ifdef DE_PRINTF
                                                                printf( "suspend\n" ); 															// sleep state

                                                                while ( XBUS_AUX & bUART0_TX )
                                                                {
                                                                    ; 	// wait for sending to complete
                                                                }
                                                                #endif
                                                                #if 0
                                                                SAFE_MOD = 0x55;
                                                                SAFE_MOD = 0xAA;
                                                                WAKE_CTRL = bWAK_BY_USB | bWAK_RXD0_LO | bWAK_RXD1_LO; // 					Wake up when USB or RXD0/1 has signal
                                                                PCON |= PD; 																// sleep
                                                                SAFE_MOD = 0x55;
                                                                SAFE_MOD = 0xAA;
                                                                WAKE_CTRL = 0x00;
                                                                #endif
                                                            }
                                                            else
                                                            {
                                                                len = 0xFF; 										/* operation failed*/
                                                            }
                                                        }
                                                        else
                                                        {
                                                            len = 0xFF; 											/* operation failed*/
                                                        }
                                                    }
                                                    else if( ( UsbSetupBuf->bRequestType & 0x1F ) == USB_REQ_RECIP_ENDP ) 			/* Set endpoint*/
                                                    {
                                                        if( ( ( ( uint16_t )UsbSetupBuf->wValueH << 8 ) | UsbSetupBuf->wValueL ) == 0x00 )
                                                        {
                                                            switch( ( ( uint16_t )UsbSetupBuf->wIndexH << 8 ) | UsbSetupBuf->wIndexL )
                                                            {
                                                                case 0x83:
                                                                    UEP3_CTRL = UEP3_CTRL & (~bUEP_T_TOG) | UEP_T_RES_STALL; /* set endpoint 3 IN STALL */
                                                                    break;
                                                                case 0x03:
                                                                    UEP3_CTRL = UEP3_CTRL & (~bUEP_R_TOG) | UEP_R_RES_STALL; /* Set endpoint 3 OUT Stall */
                                                                    break;
                                                                case 0x82:
                                                                    UEP2_CTRL = UEP2_CTRL & (~bUEP_T_TOG) | UEP_T_RES_STALL; /* set endpoint 2 IN STALL */
                                                                    break;
                                                                case 0x02:
                                                                    UEP2_CTRL = UEP2_CTRL & (~bUEP_R_TOG) | UEP_R_RES_STALL; /* Set endpoint 2 OUT Stall */
                                                                    break;
                                                                case 0x81:
                                                                    UEP1_CTRL = UEP1_CTRL & (~bUEP_T_TOG) | UEP_T_RES_STALL; /* set endpoint 1 IN STALL */
                                                                    break;
                                                                case 0x01:
                                                                    UEP1_CTRL = UEP1_CTRL & (~bUEP_R_TOG) | UEP_R_RES_STALL; /* Set endpoint 1 OUT Stall */
                                                                default:
                                                                    len = 0xFF; 									/* operation failed*/
                                                                    break;
                                                            }
                                                        }
                                                        else
                                                        {
                                                            len = 0xFF; 									/* operation failed*/
                                                        }
                                                    }
                                                    else
                                                    {
                                                        len = 0xFF; 										/* operation failed*/
                                                    }
                                                    break;
                                                                case USB_GET_STATUS:
                                                                    Ep0Buffer[0] = 0x00;
                                                                    Ep0Buffer[1] = 0x00;
                                                                    if ( SetupLen >= 2 )
                                                                    {
                                                                        len = 2;
                                                                    }
                                                                    else
                                                                    {
                                                                        len = SetupLen;
                                                                    }
                                                                    break;
                                                                default:
                                                                    len = 0xff; 													// operation failed
                                                                    break;
                        }
                    }
                }
                else
                {
                    len = 0xff; 														// packet length error
                }
                if(len == 0xff)
                {
                    SetupReq = 0xFF;
                    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;//STALL
                }
                else if(len <= DEFAULT_ENDP0_SIZE) 													// Upload data or return a 0-length packet in the status stage
                {
                    UEP0_T_LEN = len;
                    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;//The default data packet is DATA1, return the response ACK
                }
                else
                {
                    UEP0_T_LEN = 0; //Although it has not yet reached the state stage, it is preset to upload a 0-length data packet in advance to prevent the host from entering the state stage in advance
                    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;//The default data packet is DATA1, return the response ACK
                }
                break;
                                                                case UIS_TOKEN_IN | 0: 													//endpoint0 IN
                                                                    switch(SetupReq)
                                                                    {
                                                                        case USB_GET_DESCRIPTOR:
                                                                            len = SetupLen >= DEFAULT_ENDP0_SIZE ? DEFAULT_ENDP0_SIZE : SetupLen; 			// Length of this transmission
                                                                            if(pDescr == (__code uint8_t *)0xffff)
                                                                            {
                                                                                uuidcpy(Ep0Buffer, pDescr_Index, len);
                                                                            }
                                                                            else
                                                                            {
                                                                                memcpy( Ep0Buffer, pDescr + pDescr_Index, len ); 								// load upload data
                                                                            }
                                                                            SetupLen -= len;
                                                                            pDescr_Index += len;
                                                                            UEP0_T_LEN = len;
                                                                            UEP0_CTRL ^= bUEP_T_TOG; 											// Synchronization flag flipped
                                                                            break;
                                                                        case USB_SET_ADDRESS:
                                                                            if(VendorControl == 0)
                                                                            {
                                                                                USB_DEV_AD = USB_DEV_AD & bUDA_GP_BIT | SetupLen;
                                                                                UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                                                                            }
                                                                            break;
                                                                        default:
                                                                            UEP0_T_LEN = 0; 													// The status stage is completed, interrupted or forced to upload a 0-length data packet to end the control transmission
                                                                            UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                                                                            break;
                                                                    }
                                                                    break;
                                                                        case UIS_TOKEN_OUT | 0: // endpoint0 OUT
                                                                            if(SetupReq == 0x22) //Set serial port properties
                                                                            {

                                                                            }
                                                                            else
                                                                            {
                                                                                UEP0_T_LEN = 0;
                                                                                UEP0_CTRL |= UEP_R_RES_ACK | UEP_T_RES_NAK; //Status phase, respond to NAK to IN
                                                                            }
                                                                            break;

                                                                        default:
                                                                            break;
        }
        UIF_TRANSFER = 0; 														// write 0 to clear the interrupt
    }
    if(UIF_BUS_RST) 																// Device mode USB bus reset interrupt
    {
        #ifdef DE_PRINTF
        printf( "reset\n" ); 															// sleep state
        #endif
        UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
        UEP2_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK | UEP_R_RES_ACK;
        USB_DEV_AD = 0x00;
        UIF_SUSPEND = 0;
        UIF_TRANSFER = 0;
        UIF_BUS_RST = 0; 															// clear interrupt flag
        UsbConfig = 0; 		// clear configuration value
        UpPoint1_Busy = 0;
        UpPoint3_Busy = 0;

        USBOutLength = 0;
        USBOutPtr = 0;
        USBReceived = 0;

        USBOutLength_1 = 0;
        USBOutPtr_1 = 0;
        USBReceived_1 = 0;

        Mpsse_ShortLen = 0;
        Mpsse_LongLen = 0;

        Mpsse_Status = 0;
        UpPoint1_Ptr = 2;
        UpPoint3_Ptr = 2;

        Serial_Done = 0;
        USB_Require_Data = 0;
    }
    if (UIF_SUSPEND) 																//USB bus suspend/wake up completed
    {
        UIF_SUSPEND = 0;
        if ( USB_MIS_ST & bUMS_SUSPEND ) 											// suspend
        {
            #ifdef USB_SLEEP
            #ifdef DE_PRINTF
            printf( "suspend\n" ); 															// sleep state
            #endif
            while ( XBUS_AUX & bUART0_TX )
            {
                ; 	// wait for sending to complete
            }
            SAFE_MOD = 0x55;
            SAFE_MOD = 0xAA;
            WAKE_CTRL = bWAK_BY_USB | bWAK_RXD0_LO | bWAK_RXD1_LO; // 					Wake up when USB or RXD0/1 has signal
            PCON |= PD; 																// sleep
            SAFE_MOD = 0x55;
            SAFE_MOD = 0xAA;
            WAKE_CTRL = 0x00;
            #endif
        }
    }
    else 																			// Unexpected interruption, impossible situation
    {
        USB_INT_FG = 0xFF; 															// clear interrupt flag

    }
}

void SerialPort_Config()
{
    volatile uint32_t x;
    volatile uint8_t x2;

    /* P3.0 input */
    P3_DIR_PU &= ~((1 << 0));
    P3_MOD_OC &= ~((1 << 0));

    /* P3.1 output */
    P3_DIR_PU &= ((1 << 1));
    P3_MOD_OC |= ~((1 << 1));

    SM0 = 0;
    SM1 = 1;
    SM2 = 0; 																// serial port 0 uses mode 1
    //Use Timer1 as baud rate generator
    RCLK = 0; 																//UART0 receive clock
    TCLK = 0; 																//UART0 send clock
    PCON |= SMOD;
    x = 10 * FREQ_SYS / SBAUD_SET / 16;									   	// Calculation of baud rate: 16M/16/baud rate
    //If you change the main frequency, be careful not to overflow the value of x
    x2 = x % 10;
    x /= 10;
    if ( x2 >= 5 ) x ++; 													// round up

    TMOD = TMOD & ~ bT1_GATE & ~ bT1_CT & ~ MASK_T1_MOD | bT1_M1; 			//0X20 , Timer1 as 8-bit auto-reload timer
    T2MOD = T2MOD | bTMR_CLK | bT1_CLK; 										//Timer1 clock selection
    TH1 = 0 - x; 															//12MHz crystal oscillator, buad/12 is the actual baud rate to be set
    TR1 = 1; 																// start timer 1
    TI = 0;
    REN = 1; 																// serial port 0 receive enable
    ES = 1; //open serial port interrupt
    PS = 1; // interrupt priority is the highest
}

void Xtal_Enable(void) //enable external clock
{
    USB_INT_EN = 0;
    USB_CTRL = 0x06;

    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    CLOCK_CFG |= bOSC_EN_XT; //Enable external 24M crystal oscillator
    SAFE_MOD = 0x00;
    mDelaymS(50);

    // 	SAFE_MOD = 0x55;
    // 	SAFE_MOD = 0xAA;
    // 	CLOCK_CFG &= ~bOSC_EN_INT; // close internal RC
    // 	SAFE_MOD = 0x00;
    mDelaymS(250);
}

/**************************************************** *******************************
 * Function Name : Uart0_ISR()
 * Description 	: serial port receiving interrupt function, to realize circular buffer receiving
 ***************************************************** *******************************/

//Ring Buf

volatile __idata uint8_t WritePtr = 0;
volatile __idata uint8_t ReadPtr = 0;

#ifndef HARD_ESP_CTRL
__code uint8_t ESP_Boot_Sequence[] =
{
    0x07, 0x07, 0x12, 0x20,
    0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55
};
#endif

#define FAST_RECEIVE

#ifndef FAST_RECEIVE /* code that has been in disrepair for a long time, don't maintain it*/
void Uart0_ISR(void) __interrupt (INT_NO_UART0) __using 1
{
    if(RI) //receive data
    {
        if((WritePtr + 1) % sizeof(RingBuf) != ReadPtr)
        {
            // ring buffer write
            RingBuf[WritePtr++] = SBUF;
            WritePtr %= sizeof(RingBuf);
        }
        RI = 0;
    }
    if (TI)
    {
        if(USBOutPtr_1 >= USBOutLength_1)
        {
            UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_ACK;
            TI = 0;
        }
        else
        {
            uint8_t ch = Ep2Buffer[USBOutPtr_1];
            SBUF = ch;
            TI = 0;
#ifndef HARD_ESP_CTRL
            if(ESP_Boot_Sequence[Esp_Boot_Chk] == ch)
                Esp_Boot_Chk ++;
            else
                Esp_Boot_Chk = 0;

            if(Esp_Boot_Chk >= (sizeof(ESP_Boot_Sequence) - 1))
            {
                if(Esp_Require_Reset == 0)
                    Esp_Require_Reset = 1;
                Esp_Boot_Chk = 0;
            }
#endif
            USBOutPtr_1++;
        }
    }

}
#else

//Assemble received data, select register group 1, DPTR1 1.5M~150kHz~160 cycles
void Uart0_ISR(void) __interrupt (INT_NO_UART0) __using 1 __naked
{
    __asm
    push psw ;2
    push a
    push dph
    push dpl

    ReadFromSerial:
    jnb _RI, SendToSerial ;7

    mov a, _WritePtr ;2
    mov dpl, _ReadPtr

    inc a ;1
    anl dpl, #0x7f
    anl a, #0x7f ;2

    xrl a, dpl
    jz SendToSerial

    mov dph, #(_RingBuf >> 8) ;3
    mov dpl, _WritePtr ;3
    mov a, _SBUF ;2
    movx @dptr, a ;1

    inc _WritePtr ;1
    anl _WritePtr, #0x7f ;2

    SendToSerial:
    clr _RI ;2

    jnb _TI, ISR_End

    clr c
    mov a, _USBOutPtr_1
    subb a, _USBOutLength_1
    jc SerialTx

    UsbEpAck:
    mov _Serial_Done, #1
    sjmp Tx_End
    SerialTx:
    mov dph, #(_Ep4Buffer >> 8)
    mov dpl, _USBOutPtr_1
    movx a, @dptr
    mov _SBUF, a
    inc _USBOutPtr_1

    Tx_End:
    clr _TI

    ISR_End:

    pop dpl
    pop dph
    pop a
    pop psw
    reti
    __endasm;
}
#endif

//#define FAST_COPY_2
//#define FAST_COPY_1

void CLKO_Enable(void) //Open T2 output
{
    ET2 = 0;
    T2CON = 0;
    T2MOD = 0;
    T2MOD |= bTMR_CLK | bT2_CLK | T2OE;
    RCAP2H = 0xff;
    RCAP2L = 0xfe;
    TH2 = 0xff;
    TL2 = 0xfe;
    TR2 = 1;
    P1_MOD_OC &= ~(0x01); //P1.0 push-pull output
    P1_DIR_PU |= 0x01;
}

#define TMS T2EX
#define TDI MOSI
#define TDO MISO
#define TCK SCK
#define TCK_CONT SCS

void JTAG_IO_Config(void)
{
    P1_DIR_PU |= ((1 << 1) | (1 << 5) | (1 << 7));
    P1_DIR_PU &= ~((1 << 6) | (1 << 4));
    P1_MOD_OC &= ~((1 << 1) | (1 << 5) | (1 << 7) | (1 << 6) | (1 << 4));

    TMS = 0;
    TDI = 0;
    TDO = 0;
    TCK = 0;
    TCK_CONT = 0;
    /* P1.1 TMS, P1.5 TDI(MOSI), P1.7 TCK PP */
    /* P1.6 TDO(MISO) INPUT */
    /* P1.4 INPUT */
}

void Run_Test_Start()
{
    /* P1.7 INPUT, P1.4 PP */
    PIN_FUNC |= bT2_PIN_X;
    P1_DIR_PU &= ~((1 << 7));
    P1_MOD_OC &= ~((1 << 7));
    //TCK = 1;

    RCAP2L = 0xfd;

    P1_DIR_PU |= ((1 << 4));
    P1_MOD_OC &= ~((1 << 4));
}

void Run_Test_Stop()
{
    P1_DIR_PU &= ~((1 << 4));
    P1_MOD_OC &= ~((1 << 4)); // P1.4 INPUT

    RCAP2L = 0xfe;
    PIN_FUNC &= ~bT2_PIN_X;

    P1_DIR_PU |= ((1 << 7));
    P1_MOD_OC &= ~((1 << 7)); // P1.7 OUTPUT
}

#define MPSSE_IDLE 			0
#define MPSSE_RCV_LENGTH_L 	1
#define MPSSE_RCV_LENGTH_H 	2
#define MPSSE_TRANSMIT_BYTE 3
#define MPSSE_RCV_LENGTH 	4
#define MPSSE_TRANSMIT_BIT 	5
#define MPSSE_ERROR 			6
#define MPSSE_TRANSMIT_BIT_MSB 7
#define MPSSE_TMS_OUT 		8
#define MPSSE_NO_OP_1 		9
#define MPSSE_NO_OP_2 		10
#define MPSSE_TRANSMIT_BYTE_MSB 	11
#define MPSSE_RUN_TEST 	12

#define MPSSE_DEBUG 	0
#define MPSSE_HWSPI 	1

#define GOWIN_INT_FLASH_QUIRK 1

void SPI_Init()
{
    SPI0_CK_SE = 0x06;

}


//Define function return value
#ifndef SUCCESS
#define SUCCESS 0
#endif
#ifndef FAIL
#define FAIL 0xFF
#endif

//Define the timer start
#ifndef START
#define START 1
#endif
#ifndef STOP
#define STOP 0
#endif

//CH554 Timer0 clock selection
//bTMR_CLK affects Timer0&1&2 at the same time, pay attention when using it (except for timing using standard clock)
#define mTimer0Clk12DivFsys( ) (T2MOD &= ~bT0_CLK) //timer, clock=Fsys/12 T0 standard clock
#define mTimer0ClkFsys( ) (T2MOD |= bTMR_CLK | bT0_CLK) //timer, clock=Fsys
#define mTimer0Clk4DivFsys( ) (T2MOD &= ~bTMR_CLK;T2MOD |= bT0_CLK) //timer, clock=Fsys/4
#define mTimer0CountClk( ) (TMOD |= bT0_CT) //Counter, the falling edge of T0 pin is valid

//CH554 Timer0 start(SS=1)/end(SS=0)
#define mTimer0RunCTL( SS ) (TR0 = SS ? START : STOP)


#define mTimer1Clk12DivFsys( ) (T2MOD &= ~bT1_CLK) //timer, clock=Fsys/12 T1 standard clock
#define mTimer1ClkFsys( ) (T2MOD |= bTMR_CLK | bT1_CLK) //timer, clock=Fsys
#define mTimer1Clk4DivFsys( ) (T2MOD &= ~bTMR_CLK;T2MOD |= bT1_CLK) //timer, clock=Fsys/4
#define mTimer1CountClk( ) (TMOD |= bT1_CT) //Counter, the falling edge of T0 pin is valid

//CH554 Timer1 start(SS=1)/end(SS=0)
#define mTimer1RunCTL( SS ) (TR1 = SS ? START : STOP)


#define mTimer2Clk12DivFsys( ) {T2MOD &= ~ bT2_CLK;C_T2 = 0;} //timer, clock=Fsys/12 T2 standard clock
#define mTimer2ClkFsys( ) {T2MOD |= (bTMR_CLK | bT2_CLK);C_T2=0;} //timer, clock=Fsys
#define mTimer2Clk4DivFsys( ) {T2MOD &= ~bTMR_CLK;T2MOD |= bT2_CLK;C_T2 = 0;}//timer, clock=Fsys/4
#define mTimer2CountClk( ) {C_T2 = 1;} //counter, the falling edge of T2 pin is valid

//CH554 Timer2 start(SS=1)/end(SS=0)
#define mTimer2RunCTL( SS ) {TR2 = SS ? START : STOP;}
#define mTimer2OutCTL( ) (T2MOD |= T2OE) //T2 output frequency TF2/2
#define CAP1Alter( ) (PIN_FUNC |= bT2_PIN_X;) //CAP1 is mapped from P10 to P14
#define CAP2Alter( ) (PIN_FUNC |= bT2EX_PIN_X;) //CAP2 is mapped to RST by P11

/**************************************************** *******************************
 * Function Name : mTimer_x_ModInit(uint8_t x ,uint8_t mode)
 * Description : CH554 timer counter x mode setting
 * Input : uint8_t mode, Timer mode selection
 * 0 *: Mode 0, 13-bit timer, the upper 3 bits of TLn are invalid
 * 1: Mode 1, 16-bit timer
 * 2: Mode 2, 8-bit auto-reload timer
 * 3: Mode 3, two 8-bit timers Timer0
 * 3: Mode 3, Timer1 stops
 * uint8_t x Timer 0 1 2
 * Output : None
 * Return : SUCCESS
 * FAIL
 ***************************************************** *******************************/
uint8_t mTimer_x_ModInit(uint8_t x ,uint8_t mode);

/**************************************************** *******************************
 * Function Name : mTimer_x_SetData(uint8_t x,uint16_t dat)
 * Description : CH554Timer
 * Input : uint16_t dat; timer assignment
 * u *int8_t x Timer 0 1 2
 * Output : None
 * Return : None
 ***************************************************** *******************************/
void mTimer_x_SetData(uint8_t x,uint16_t dat);

/**************************************************** *******************************
 * Function Name : CAP2Init(uint8_t mode)
 * Description : CH554 timing counter 2 T2EX pin capture function initialization
 * u *int8_t mode, edge capture mode selection
 * 0: T2ex from falling edge to next falling edge
 * 1: Between any edge of T2ex
 * 3: T2ex from rising edge to next rising edge
 * Input : None
 * Output : None
 * Return : None
 ***************************************************** *******************************/
void CAP2Init(uint8_t mode);

/**************************************************** *******************************
 * Function Name : CAP1Init(uint8_t mode)
 * Description : CH554 timing counter 2 T2 pin capture function initializes T2
 * u *int8_t mode, edge capture mode selection
 * 0: T2ex from falling edge to next falling edge
 * 1: Between any edge of T2ex
 * 3: T2ex from rising edge to next rising edge
 * Input : None
 * Output : None
 * Return : None
 ***************************************************** *******************************/
void CAP1Init(uint8_t mode);

/**************************************************** *******************************
 * Function Name : mTimer_x_ModInit(uint8_t x ,uint8_t mode)
 * Description : CH554 timer counter x mode setting
 * Input : uint8_t mode, Timer mode selection
 * 0 *: Mode 0, 13-bit timer, the upper 3 bits of TLn are invalid
 * 1: Mode 1, 16-bit timer
 * 2: Mode 2, 8-bit auto-reload timer
 * 3: Mode 3, two 8-bit timers Timer0
 * 3: Mode 3, Timer1 stops
 * Output : None
 * Return : SUCCESS
 * FAIL
 ***************************************************** *******************************/
uint8_t mTimer_x_ModInit(uint8_t x ,uint8_t mode)
{
    if(x == 0){TMOD = TMOD & 0xf0 | mode;}
    else if(x == 1){TMOD = TMOD & 0x0f | (mode<<4);}
    else if(x == 2){RCLK = 0;TCLK = 0;CP_RL2 = 0;} //16-bit auto-reload timer
    else return FAIL;
    return SUCCESS;
}

/**************************************************** *******************************
 * Function Name : mTimer_x_SetData(uint8_t x,uint16_t dat)
 * Description : CH554Timer0 TH0 and TL0 assignment
 * Input : uint16_t dat; timer assignment
 * Output : None
 * Return : None
 ***************************************************** *******************************/
void mTimer_x_SetData(uint8_t x,uint16_t dat)
{
    uint16_t tmp;
    tmp = 65536-dat;
    if(x == 0){TL0 = tmp & 0xff;TH0 = (tmp>>8) & 0xff;}
    else if(x == 1){TL1 = tmp & 0xff;TH1 = (tmp>>8) & 0xff;}
    else if(x == 2){
        RCAP2L = TL2 = tmp & 0xff; //16-bit auto-reload timer
        RCAP2H = TH2 = (tmp>>8) & 0xff;
    }
}

/**************************************************** *******************************
 * Function Name : CAP2Init(uint8_t mode)
 * Description : CH554 timing counter 2 T2EX pin capture function initialization
 * u *int8_t mode, edge capture mode selection
 * 0: T2ex from falling edge to next falling edge
 * 1: Between any edge of T2ex
 * 3: T2ex from rising edge to next rising edge
 * Input : None
 * Output : None
 * Return : None
 ***************************************************** *******************************/
void CAP2Init(uint8_t mode)
{
    RCLK = 0;
    TCLK = 0;
    C_T2 = 0;
    EXEN2 = 1;
    CP_RL2 = 1; //Start the capture function of T2ex
    T2MOD |= mode << 2; //Edge capture mode selection
}

/**************************************************** *******************************
 * Function Name : CAP1Init(uint8_t mode)
 * Description : CH554 timing counter 2 T2 pin capture function initializes T2
 * u *int8_t mode, edge capture mode selection
 * 0: T2ex from falling edge to next falling edge
 * 1: Between any edge of T2ex
 * 3: T2ex from rising edge to next rising edge
 * Input : None
 * Output : None
 * Return : None
 ***************************************************** *******************************/
void CAP1Init(uint8_t mode)
{
    RCLK = 0;
    TCLK = 0;
    CP_RL2 = 1;
    C_T2 = 0;
    T2MOD = T2MOD & ~T2OE | (mode << 2) | bT2_CAP1_EN; //Enable T2 pin capture function, edge capture mode selection
}


/**************************************************** *******************************
 * Function Name : mTimer0Interrupt()
 * Description : CH554 timer counter 0 timer counter interrupt processing function
 ***************************************************** *******************************/
void mTimer0Interrupt(void) __interrupt (INT_NO_TMR0) //timer0 interrupt service routine
{
    mTimer_x_SetData(0,1000); //Non-automatic reload mode needs to reassign TH0 and TL0, 1MHz/1000=1000Hz, 1ms
    SOF_Count ++;
    if(Modem_Count)
        Modem_Count --;
    if(Modem_Count == 1)
    {
        if(soft_dtr == 0 && soft_rts == 1)
        {
            INTF1_RTS = 1;
            INTF1_DTR = 0;
        }
        if(soft_dtr == 1 && soft_rts == 0)
        {
            INTF1_RTS = 0;
            INTF1_DTR = 1;
        }
        if(soft_dtr == soft_rts)
        {
            INTF1_DTR = 1;
            INTF1_RTS = 0;
            INTF1_RTS = 1;
        }
    }
    if(SOF_Count % 16 == 0)
        PWM2 = 1;
}

void init_timer() {
    mTimer0Clk12DivFsys(); 	//T0 timer clock setting, 12MHz/12=1MHz
    mTimer_x_ModInit(0,1); //T0 timer mode setting
    mTimer_x_SetData(0,1000); 	//T0 timer assignment, 1MHz/1000=1000Hz, 1ms
    mTimer0RunCTL(1); //T0 timer start
    ET0 = 1; //T0 timer interrupt enabled
    EA = 1;

    SOF_Count = 0;
}

#if MPSSE_HWSPI
#define SPI_LSBFIRST() SPI0_SETUP |= bS0_BIT_ORDER
#define SPI_MSBFIRST() SPI0_SETUP &= ~bS0_BIT_ORDER
#define SPI_ON() SPI0_CTRL = bS0_MISO_OE | bS0_MOSI_OE | bS0_SCK_OE;
#define SPI_OFF() SPI0_CTRL = 0;
#else
#define SPI_LSBFIRST()
#define SPI_MSBFIRST()
#define SPI_ON()
#define SPI_OFF()
#endif
//main function
main()
{
    uint8_t i;
    uint8_t Purge_Buffer = 0;
    uint8_t data, rcvdata;
    uint8_t instr = 0;
    volatile uint16_t Uart_Timeout = 0;
    volatile uint16_t Uart_Timeout1 = 0;
    uint16_t Esp_Stage = 0;
    // int8_t size;


    Xtal_Enable(); 	// Start the oscillator
    CfgFsys( ); 														//CH552 clock selection configuration
    mDelaymS(5); 														// Modify the main frequency and wait for the internal clock to stabilize, it must be added
    CLKO_Enable();
    JTAG_IO_Config();
    SerialPort_Config();

    PWM2 = 1;

    #if MPSSE_HWSPI
    SPI_Init();
    #endif

    #ifdef DE_PRINTF
    printf("start...\n");
    #endif
    USBDeviceCfg();
    USBDeviceEndPointCfg(); 											// endpoint configuration
    USBDeviceIntCfg(); 													// Interrupt initialization
    UEP0_T_LEN = 0;
    UEP1_T_LEN = 0; 													// The pre-used sending length must be cleared
    UEP2_T_LEN = 0; 													// The pre-used sending length must be cleared

    /* pre-populate Modem Status */
    Ep1Buffer[0] = 0x01;
    Ep1Buffer[1] = 0x60;
    Ep3Buffer[0] = 0x01;
    Ep3Buffer[1] = 0x60;
    UpPoint1_Ptr = 2;
    UpPoint3_Ptr = 2;
    XBUS_AUX = 0;
    #ifndef SOF_NO_TIMER
    init_timer(); // add 1 every 1ms SOF_Count
    #endif
    T1 = 0;
    while(1)
    {
        if(UsbConfig)
        {
            if(USBReceived == 1)
            { //received a packet
                #if MPSSE_DEBUG
                if(UpPoint1_Ptr < 64 && UpPoint1_Busy == 0 && UpPoint3_Busy == 0 && UpPoint3_Ptr < 64) /* can send */
                    #else
                    if(UpPoint1_Ptr < 64 && UpPoint1_Busy == 0)
                        #endif
                    {
                        PWM2 = !PWM2;
                        switch(Mpsse_Status)
                        {
                            case MPSSE_IDLE:
                                instr = Ep2Buffer[USBOutPtr];
                                #if MPSSE_DEBUG
                                Ep3Buffer[UpPoint3_Ptr++] = instr;
                                #endif
                                switch(instr)
                                {
                                    case 0x80:
                                    case 0x82: /* fake Bit bang mode*/
                                        Mpsse_Status = MPSSE_NO_OP_1;
                                        USBOutPtr++;
                                        break;
                                    case 0x81:
                                    case 0x83: /* false state */
                                        Ep1Buffer[UpPoint1_Ptr++] = Ep2Buffer[USBOutPtr] - 0x80;
                                        USBOutPtr++;
                                        break;
                                    case 0x84:
                                    case 0x85: /* Loopback */
                                        USBOutPtr++;
                                        break;
                                    case 0x86: /* Speed adjustment, temporarily not supported*/
                                        Mpsse_Status = MPSSE_NO_OP_1;
                                        USBOutPtr++;
                                        break;
                                    case 0x87: /* flush the buffer immediately*/
                                        Purge_Buffer = 1;
                                        USBOutPtr++;
                                        break;
                                    case 0x19:
                                    case 0x39:
                                    case 0x11:
                                    case 0x31:
                                        SPI_ON();
                                        Mpsse_Status = MPSSE_RCV_LENGTH_L;
                                        USBOutPtr++;
                                        break;
                                    case 0x6b:
                                    case 0x4b:
                                    case 0x3b:
                                    case 0x1b:
                                    case 0x13:
                                        SPI_OFF();
                                        Mpsse_Status = MPSSE_RCV_LENGTH;
                                        USBOutPtr++;
                                        break;
                                    default: 	/* unsupported command*/
                                        Ep1Buffer[UpPoint1_Ptr++] = 0xfa;
                                        Mpsse_Status = MPSSE_ERROR;
                                        break;
                                }
                                break;
                                    case MPSSE_RCV_LENGTH_L: /* Receive length*/
                                        Mpsse_LongLen = Ep2Buffer[USBOutPtr];
                                        Mpsse_Status++;
                                        USBOutPtr++;
                                        break;
                                    case MPSSE_RCV_LENGTH_H:
                                        Mpsse_LongLen |= (Ep2Buffer[USBOutPtr] << 8) & 0xff00;
                                        USBOutPtr++;
                                        #if GOWIN_INT_FLASH_QUIRK
                                        if((Mpsse_LongLen == 25000 || Mpsse_LongLen == 750 || Mpsse_LongLen == 2968) && (instr & (1 << 5)) == 0)
                                        {
                                            SPI_OFF();
                                            Run_Test_Start();
                                            Mpsse_Status = MPSSE_RUN_TEST;
                                        }
                                        else if(instr == 0x11 || instr == 0x31)
                                            #else
                                            if (instr == 0x11 || instr == 0x31)
                                                #endif
                                            {
                                                Mpsse_Status = MPSSE_TRANSMIT_BYTE_MSB;
                                                SPI_MSBFIRST();
                                            }
                                            else
                                            {
                                                Mpsse_Status++;
                                                SPI_LSBFIRST();
                                            }
                                            break;
                                    case MPSSE_TRANSMIT_BYTE:
                                        data = Ep2Buffer[USBOutPtr];
                                        #if MPSSE_HWSPI
                                        SPI0_DATA = data;
                                        while(S0_FREE == 0);
                                        rcvdata = SPI0_DATA;
                            #else
                            rcvdata = 0;
                            for(i = 0; i < 8; i++)
                            {
                                SCK = 0;
                                MOSI = (data & 0x01);
                                data >>= 1;
                                rcvdata >>= 1;
                                __asm nop __endasm;
                                __asm nop __endasm;
                                SCK = 1;
                                if(MISO == 1)
                                    rcvdata |= 0x80;
                                __asm nop __endasm;
                                __asm nop __endasm;
                            }
                            SCK = 0;
                            #endif
                            if(instr == 0x39)
                                Ep1Buffer[UpPoint1_Ptr++] = rcvdata;
                            USBOutPtr++;
                            if(Mpsse_LongLen == 0)
                                Mpsse_Status = MPSSE_IDLE;
                            Mpsse_LongLen --;
                            break;
                                    case MPSSE_TRANSMIT_BYTE_MSB:
                                        data = Ep2Buffer[USBOutPtr];
                                        #if MPSSE_HWSPI
                                        SPI0_DATA = data;
                                        while(S0_FREE == 0);
                                        rcvdata = SPI0_DATA;
                            #else
                            rcvdata = 0;
                            for(i = 0; i < 8; i++)
                            {
                                SCK = 0;
                                MOSI = (data & 0x80);
                                data <<= 1;
                                rcvdata <<= 1;
                                __asm nop __endasm;
                                __asm nop __endasm;
                                SCK = 1;
                                if(MISO == 1)
                                    rcvdata |= 0x01;
                                __asm nop __endasm;
                                __asm nop __endasm;
                            }
                            SCK = 0;
                            #endif
                            if(instr == 0x31)
                                Ep1Buffer[UpPoint1_Ptr++] = rcvdata;
                            USBOutPtr++;
                            if(Mpsse_LongLen == 0)
                                Mpsse_Status = MPSSE_IDLE;
                            Mpsse_LongLen --;
                            break;
                                    case MPSSE_RCV_LENGTH:
                                        Mpsse_ShortLen = Ep2Buffer[USBOutPtr];
                                        if(instr == 0x6b || instr == 0x4b)
                                            Mpsse_Status = MPSSE_TMS_OUT;
                            else if(instr == 0x13)
                                Mpsse_Status = MPSSE_TRANSMIT_BIT_MSB;
                            else
                                Mpsse_Status++;
                            USBOutPtr++;
                            break;
                                    case MPSSE_TRANSMIT_BIT:
                                        data = Ep2Buffer[USBOutPtr];
                                        rcvdata = 0;
                                        do
                                        {
                                            SCK = 0;
                                            MOSI = (data & 0x01);
                                            data >>= 1;
                                            rcvdata >>= 1;
                                            __asm nop __endasm;
                                            __asm nop __endasm;
                                            SCK = 1;
                                            if(MISO)
                                                rcvdata |= 0x80;//(1 << (Mpsse_ShortLen));
                                            __asm nop __endasm;
                                            __asm nop __endasm;
                                        } while((Mpsse_ShortLen--) > 0);
                                        SCK = 0;
                                        if(instr == 0x3b)
                                            Ep1Buffer[UpPoint1_Ptr++] = rcvdata;
                            Mpsse_Status = MPSSE_IDLE;
                            USBOutPtr++;
                            break;
                                    case MPSSE_TRANSMIT_BIT_MSB:
                                        data = Ep2Buffer[USBOutPtr];
                                        rcvdata = 0;
                                        do
                                        {
                                            SCK = 0;
                                            MOSI = (data & 0x80);
                                            data <<= 1;
                                            __asm nop __endasm;
                                            __asm nop __endasm;
                                            SCK = 1;
                                            __asm nop __endasm;
                                            __asm nop __endasm;
                                        } while((Mpsse_ShortLen--) > 0);
                                        SCK = 0;

                                        Mpsse_Status = MPSSE_IDLE;
                                        USBOutPtr++;

                                        break;
                                    case MPSSE_ERROR:
                                        Ep1Buffer[UpPoint1_Ptr++] = Ep2Buffer[USBOutPtr];
                                        Mpsse_Status = MPSSE_IDLE;
                                        USBOutPtr++;
                                        break;
                                    case MPSSE_TMS_OUT:
                                        data = Ep2Buffer[USBOutPtr];
                                        if(data & 0x80)
                                            TDI = 1;
                            else
                                TDI = 0;
                            rcvdata = 0;
                            do
                            {
                                TCK = 0;
                                TMS = (data & 0x01);
                                data >>= 1;
                                rcvdata >>= 1;
                                __asm nop __endasm;
                                __asm nop __endasm;
                                SCK = 1;
                                if(TDO)
                                    rcvdata |= 0x80;//(1 << (Mpsse_ShortLen));
                                __asm nop __endasm;
                                __asm nop __endasm;
                            } while((Mpsse_ShortLen--) > 0);
                            TCK = 0;
                            if(instr == 0x6b)
                                Ep1Buffer[UpPoint1_Ptr++] = rcvdata;
                            Mpsse_Status = MPSSE_IDLE;
                            USBOutPtr++;
                            break;
                                    case MPSSE_NO_OP_1:
                                        Mpsse_Status++;
                                        USBOutPtr++;
                                        break;
                                    case MPSSE_NO_OP_2:
                                        Mpsse_Status = MPSSE_IDLE;
                                        USBOutPtr++;
                                        break;
                                        #if GOWIN_INT_FLASH_QUIRK
                                    case MPSSE_RUN_TEST:
                                        if(Mpsse_LongLen == 0)
                                        {
                                            Mpsse_Status = MPSSE_IDLE;
                                            Run_Test_Stop();
                                        }

                                        USBOutPtr++;
                                        Mpsse_LongLen --;
                                        break;
                                        #endif
                                    default:
                                        Mpsse_Status = MPSSE_IDLE;
                                        break;
                        }


                        if(USBOutPtr >= USBOutLength)
                        { // Received
                            USBReceived = 0;
                            UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_ACK;
                            // open receive
                        }
                    }
            }

            if(UpPoint1_Busy == 0)
            {
                if(UpPoint1_Ptr == 64)
                {
                    UpPoint1_Busy = 1;
                    UEP1_T_LEN = 64;
                    UEP1_CTRL = UEP1_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;
                    UpPoint1_Ptr = 2;
                }
                else if((uint16_t) (SOF_Count - Uart_Timeout) >= Latency_Timer || Purge_Buffer == 1) //timeout
                {
                    Uart_Timeout = SOF_Count;

                    UpPoint1_Busy = 1;
                    UEP1_T_LEN = UpPoint1_Ptr;
                    UEP1_CTRL = UEP1_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK; 			// Response ACK
                    UpPoint1_Ptr = 2;
                    Purge_Buffer = 0;
                }
            }

            if(UpPoint3_Busy == 0)
            {
                int8_t size = WritePtr - ReadPtr;
                if(size < 0) size = size + sizeof(RingBuf);//find the remainder

                if(size >= 62)
                {
                    for(i = 0; i < 62; i++)
                    {
                        Ep3Buffer[2 + i] = RingBuf[ReadPtr++];
                        ReadPtr %= sizeof(RingBuf);
                    }
                    UpPoint3_Busy = 1;
                    UEP3_T_LEN = 64;
                    UEP3_CTRL = UEP3_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;
                    UpPoint3_Ptr = 2;

                }
                else if((uint16_t) (SOF_Count - Uart_Timeout1) >= Latency_Timer1) //timeout
                {
                    Uart_Timeout1 = SOF_Count;
                    if(size > 62) size = 62;
                    for(i = 0; i < (uint8_t)size; i++)
                    {
                        Ep3Buffer[2 + i] = RingBuf[ReadPtr++];
                        ReadPtr %= sizeof(RingBuf);
                    }
                    UpPoint3_Busy = 1;
                    // UEP3_T_LEN = UpPoint3_Ptr;
                    UEP3_T_LEN = 2 + size;
                    UpPoint3_Ptr = 2;
                    UEP3_CTRL = UEP3_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK; 			// Response ACK
                }
            }

            if(USBReceived_1) //IDLE state
            {
                if(Serial_Done == 0) //serial port IDLE
                {
                    Serial_Done = 2; //serial port sending
                    TI = 1;
                }
                if(UEP4_CTRL & MASK_UEP_R_RES != UEP_R_RES_ACK)
                    UEP4_CTRL = UEP4_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_ACK;
                USBReceived_1 = 0;
            }

            if(Serial_Done == 1)
            {
                Serial_Done = 2; //serial port sending
                TI = 1;

                Serial_Done = 0;
                //if(UEP4_CTRL & MASK_UEP_R_RES != UEP_R_RES_ACK)
                UEP4_CTRL = UEP4_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_ACK;
            }

            if(Require_DFU)
            {
                Require_DFU = 0;
                Jump_to_BL();
            }
        }
    }
}
