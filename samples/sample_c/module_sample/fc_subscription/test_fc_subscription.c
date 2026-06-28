/**
 ********************************************************************
 * @file    test_fc_subscription.c
 * @brief
 *
 * @copyright (c) 2021 DJI. All rights reserved.
 *
 * All information contained herein is, and remains, the property of DJI.
 * The intellectual and technical concepts contained herein are proprietary
 * to DJI and may be covered bDjiLowSpeedDataChannel_RegRecvDataCallback(DJI_CHANNEL_ADDRESS_MASTER_RC_APP,
    (DjiLowSpeedDataChannelRecvDataCallback)OnTextInputReceived);
y U.S. and foreign patents, patents in process,
 * and protected by trade secret or copyright law.  Dissemination of this
 * information, including but not limited to data and other proprietary
 * material(s) incorporated within the information, in any form, is strictly
 * prohibited without the express written consent of DJI.
 *
 * If you receive this source code without DJI’s authorization, you may not
 * further disseminate the information, and you must immediately remove the
 * source code and notify DJI of its removal. DJI reserves the right to pursue
 * legal actions against you for any loss(es) or damage(s) caused by your
 * failure to do so.
 *
 *********************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include <utils/util_misc.h>
#include <math.h>
#include "test_fc_subscription.h"
#include "dji_logger.h"
#include "dji_platform.h"
#include "widget_interaction_test/test_widget_interaction.h"
#include "widget/test_widget.h"
#include "dji_widget.h"
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include "dji_low_speed_data_channel.h"
#include <sys/stat.h>
#include <time.h>
#include "logger/logger.h"
#include <stdatomic.h>


/* Private constants ---------------------------------------------------------*/
#define FC_SUBSCRIPTION_TASK_FREQ         (1)
#define FC_SUBSCRIPTION_TASK_STACK_SIZE   (1024)

#define DJI_LOW_SPEED_DATA_CHANNEL_TEXT 1


/* Private types -------------------------------------------------------------*/

/* Private functions declaration ---------------------------------------------*/
static void *UserFcSubscription_Task(void *arg);
static T_DjiReturnCode DjiTest_FcSubscriptionReceiveQuaternionCallback(const uint8_t *data, uint16_t dataSize,
                                                                       const T_DjiDataTimestamp *timestamp);

/* Private variables ---------------------------------------------------------*/
static T_DjiTaskHandle s_userFcSubscriptionThread;
static bool s_userFcSubscriptionDataShow = false;
static uint8_t s_totalSatelliteNumberUsed = 0;
static uint32_t s_userFcSubscriptionDataCnt = 0;
_Atomic bool logging_active = false; // already declared in your sample
static int32_t s_logging_widget_state = 0; // to hold the widget's state
static bool save_csv = false;
static bool waiting_for_csv_name = true;
FILE *g_csvFile = NULL;
char fileName[256] = {0};
_Atomic bool exit_script_and_shutdown = false;
_Atomic(uint8_t) g_sampleRateMode = 0;

bool simulate_fc = false;

/* at file top --------------------------------------------------- */
static bool g_simulate_fc = false;   /* set by env-var later */

/* wrapper that callers will use --------------------------------- */
static T_DjiReturnCode GetTopicLatestOrSimulate(uint16_t  topic,
                                                uint8_t  *buf,
                                                uint16_t  size,
                                                T_DjiDataTimestamp *ts)
{
    if (!g_simulate_fc) {
        return DjiFcSubscription_GetLatestValueOfTopic(topic, buf, size, ts);
    }

    /* ---- fake payload when SIM is on -------------------------- */
    if (buf) memset(buf, 0, size);
    if (ts)  memset(ts,  0, sizeof(*ts));
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}


__attribute__((constructor)) 
static void read_env_flag(void) {
    const char *e = getenv("SIM");
    if (e && (*e == '1' || *e == 'y' || *e == 'Y')) {
        simulate_fc = true;
	} 
}


void startLogging(void)
{
    atomic_store_explicit(&logging_active, true, memory_order_relaxed);
    USER_LOG_INFO("Logging started.");
}

void stopLogging(void)
{
	atomic_store_explicit(&logging_active, false, memory_order_relaxed);
    USER_LOG_INFO("Logging stopped.");
}

void exitScriptSaveCsv(void)
{
    save_csv = true;
    USER_LOG_INFO("Saving Script, exiting script.");
}

// New callback function to process text input (assumes UTF-8 encoded null-terminated string)
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// Callback for text input; must return uint32_t

/* --------------------------------------------------------------------
 * Callback : request save of the stopped 400 Hz fusion log
 * ------------------------------------------------------------------*/
uint32_t OnTextInputReceived(const uint8_t *data, uint16_t dataSize)
{
    /* ---------- grab the user text -------------------------------- */
    size_t n = (dataSize < sizeof(fileName) - 1) ? dataSize
                                                 : sizeof(fileName) - 1;
    memcpy(fileName, data, n);
    fileName[n] = '\0';

    /* always add “.csv” if missing                                   */
    if (!strstr(fileName, ".csv"))
        strncat(fileName, ".csv",
                sizeof(fileName) - strlen(fileName) - 1);

    USER_LOG_INFO("Received CSV file name: %s", fileName);
    DjiTest_WidgetLogAppend("Received CSV file name: %s", fileName);
    waiting_for_csv_name = false;

    /* ---------- build destination path ---------------------------- */
    char base[256] = {0}, ext[64] = {0};
    const char *dot = strrchr(fileName, '.');
    if (dot) {
        size_t blen = dot - fileName;
        if (blen >= sizeof(base)) blen = sizeof(base) - 1;
        strncpy(base, fileName, blen);
        strncpy(ext,  dot, sizeof(ext) - 1);        /* keep the '.' */
    } else {
        strncpy(base, fileName, sizeof(base) - 1);  /* unlikely */
    }

    time_t now = time(NULL);
    struct tm utc_tm;  gmtime_r(&now, &utc_tm);
    char utc[32];
    strftime(utc, sizeof utc, "_%Y_%m_%d_%H_%M_%S", &utc_tm);

    char dest[256];
    snprintf(dest, sizeof dest,
             "/home/rmbl/Desktop/Collected Data/%s%s%s", base, utc, ext);

    /* avoid collisions by appending _1, _2, … */
    struct stat st;
    int ctr = 1;
    while (stat(dest, &st) == 0) {
        snprintf(dest, sizeof dest,
                 "/home/rmbl/Desktop/Collected Data/%s%s_%d%s",
                 base, utc, ctr++, ext);
    }

    if (atomic_load_explicit(&logging_active, memory_order_relaxed)) {
        DjiTest_WidgetLogAppend("Stop logging before saving CSV");
        return 0;
    }

    DjiTest_WidgetLogAppend("CSV save requested");
    logger_request_save_csv(dest);
    return 0;
}

T_DjiReturnCode OnWidgetSetValue(E_DjiWidgetType type, uint32_t index, int32_t value, void *userData)
{
    switch (index) {
        case 1:  // Button 1: Start logging
            if (value == 1) {
                if (logger_has_unsaved_session()) {
                    DjiTest_WidgetLogAppend("Save current CSV before starting a new session.");
                    break;
                }
                waiting_for_csv_name = true;
                startLogging();
                DjiTest_WidgetLogAppend("Logging Data");
            }
            break;
        case 2:  // Button 2: Stop logging
            if (value == 1) {
                logging_active = false;
                DjiTest_WidgetLogAppend("Logging Stopped, to save CSV enter file name and hit send in settings");
            }
            break;
        case 3:  // Button 3: Shutdown and exit
            if (value == 1) {
                if (logging_active) {
                    DjiTest_WidgetLogAppend("Cannot Shutdown and Exit while Data is logging");
                } else if (waiting_for_csv_name == true) {
                    DjiTest_WidgetLogAppend("Any unsaved data will be lost! Press again to shutdown");
                    waiting_for_csv_name = false;
                } else {
                    atomic_store_explicit(&exit_script_and_shutdown, true, memory_order_relaxed);
                    DjiTest_WidgetLogAppend("Shutting Down!");
                }
            }
            break;
        case 4:
            if (value == 1) {
                // advance the mode (0→1→2→0 if TRI_MODE_COUNT==3)
                uint8_t old = atomic_load_explicit(&g_triDisplayMode, memory_order_relaxed);
                uint8_t next = (old + 1) % TRI_MODE_COUNT;
                atomic_store_explicit(&g_triDisplayMode, next, memory_order_relaxed);

                // show which field is now active
                switch (next) {
                    case 0:
                        DjiTest_WidgetLogAppend("Now showing: Temperature");
                        break;
                    case 1:
                        DjiTest_WidgetLogAppend("Now showing: Wind Speed");
                        break;
                    case 2:
                        DjiTest_WidgetLogAppend("Now showing: Humidity");
                        break;
                    // add more cases if you enlarged TRI_MODE_COUNT
                }
            }
            break;
		case 5:
			uint8_t old = atomic_load_explicit(&g_sampleRateMode, memory_order_relaxed);
			uint8_t next = (old + 1) % SAMPLE_RATE_COUNT;
			atomic_store_explicit(&g_sampleRateMode, next,  memory_order_relaxed);
			tri_request_output_rate(next);
				
			static const char *msg[SAMPLE_RATE_COUNT] = {
					"Sample rate: 5 Hz", 
					"Sample rate: 10 Hz", 
					"Sample rate: 20 Hz", 
					"Sample rate: 40 Hz"
			};
			DjiTest_WidgetLogAppend("%s", msg[next]);
				
			break;
        default:
            USER_LOG_INFO("Unknown widget index.");
            break;
    }
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode OnWidgetGetValue(E_DjiWidgetType type, uint32_t index, int32_t *value, void *userData)
{
    switch (index) {
			case 1: *value = atomic_load_explicit(&logging_active, memory_order_relaxed); break;
			case 5: *value = (int32_t)atomic_load_explicit(&g_sampleRateMode, memory_order_relaxed); break;
			default: *value = 0; break;
	}
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}



T_DjiReturnCode DjiTest_FcSubscriptionRunSample(void)
{
    T_DjiReturnCode djiStat;
    T_DjiOsalHandler *osalHandler = DjiPlatform_GetOsalHandler();
    T_DjiDataTimestamp timestamp = {0};

    // Original topics
    T_DjiFcSubscriptionQuaternion quaternion = {0};
    T_DjiFcSubscriptionGpsPosition gpsPosition = {0};
    T_DjiFcSubscriptionGpsTime gpsTime = 0;

    // New topics – use the SDK-provided types:
    T_DjiFcSubscriptionAccelerationRaw accGround = {0};
    T_DjiFcSubscriptionAccelerationRaw accBody   = {0};
    T_DjiFcSubscriptionAccelerationRaw accRaw    = {0};

    T_DjiFcSubscriptionVelocity velocity = {0};

    // Angular rate topics
    T_DjiFcSubscriptionAngularRateFusioned angRateFusioned = {0};
    T_DjiFcSubscriptionAngularRateRaw angRateRaw       = {0};

    // Altitude topics (floats)
    T_DjiFcSubscriptionAltitudeFused altFused = 0;
    T_DjiFcSubscriptionAltitudeBarometer altBarometer = 0;
    T_DjiFcSubscriptionAltitudeOfHomePoint altHome = 0;

    // Height topics (floats)
    T_DjiFcSubscriptionHeightFusion heightFusion = 0;
    T_DjiFcSubscriptionHeightRelative heightRelative = 0;

    // Fused position structure: longitude, latitude, altitude, and satellite count.
    T_DjiFcSubscriptionPositionFused posFused = {0};

    T_DjiFcSubscriptionGpsVelocity gpsVel = {0};

    // RTK topics.
    T_DjiFcSubscriptionRtkPosition rtkPosition = {0};
    T_DjiFcSubscriptionRtkVelocity rtkVelocity = {0};
    T_DjiFcSubscriptionRtkYaw rtkYaw = 0;
    T_DjiFcSubscriptionRtkPositionInfo rtkPosInfo = 0;

    T_DjiFcSubscriptionRTKConnectStatus rtkConnectStatus = {0};

    // Compass.
    T_DjiFcSubscriptionCompass compass = {0};

    // Flight anomaly.
    T_DjiFcSubscriptionFlightAnomaly flightAnomaly = {0};

    // Position VO.
    T_DjiFcSubscriptionPositionVO posVo = {0};

    // Obstacle avoidance data.
    T_DjiFcSubscriptionAvoidData avoidData = {0};

    USER_LOG_INFO("Fc subscription sample start");
	s_userFcSubscriptionDataShow = true;

	USER_LOG_INFO("--> Step 1: Init fc subscription module");
	djiStat = DjiFcSubscription_Init();
	if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
		USER_LOG_ERROR("Init data subscription module error.");
	    return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	}
	if(!simulate_fc) {
	    USER_LOG_INFO("--> Step 2: Subscribe to all topics at 5Hz");

	    // Subscribe original topics.
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_QUATERNION,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_200_HZ,
	                                               DjiTest_FcSubscriptionReceiveQuaternionCallback);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic quaternion error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_GPS_POSITION,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_5_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic GPS position error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_GPS_TIME,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_5_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic GPS time error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }

	    // Subscribe new topics.
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_ACCELERATION_GROUND,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_200_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic acceleration ground error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_ACCELERATION_BODY,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_200_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic acceleration body error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_ACCELERATION_RAW,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_400_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic acceleration raw error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_VELOCITY,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_200_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic velocity error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_ANGULAR_RATE_FUSIONED,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_200_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic angular rate fusioned error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_ANGULAR_RATE_RAW,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_400_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic angular rate raw error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_ALTITUDE_FUSED,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_200_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic altitude fused error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_ALTITUDE_BAROMETER,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_200_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic altitude barometer error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_ALTITUDE_OF_HOMEPOINT,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_1_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic altitude of homepoint error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_HEIGHT_FUSION,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_100_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic height fusion error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_HEIGHT_RELATIVE,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_200_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic height relative error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_POSITION_FUSED,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_200_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic position fused error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_GPS_VELOCITY,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_5_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic GPS velocity error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_GPS_DATE,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_5_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic GPS velocity error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_RTK_POSITION,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_5_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic RTK position error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_RTK_VELOCITY,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_5_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic RTK velocity error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_RTK_YAW,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_5_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic RTK yaw error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_RTK_POSITION_INFO,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_5_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic RTK position info error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_COMPASS,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_100_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic compass error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_RTK_CONNECT_STATUS,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_50_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic RTK connect status error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_FLIGHT_ANOMALY,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_50_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic flight anomaly error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_POSITION_VO,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_200_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
	        USER_LOG_ERROR("Subscribe topic position VO error.");
	        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_AVOID_DATA,
	                                               DJI_DATA_SUBSCRIPTION_TOPIC_100_HZ, NULL);
	    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_ERROR("Subscribe topic avoid data error.");
	    return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
	    }
	

        // (Optional) Widget initialization.
        T_DjiReturnCode widgetStat;
        widgetStat = DjiWidget_Init();
        if (widgetStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_ERROR("Widget init failed, error code: %d", widgetStat);
        }
        widgetStat = DjiWidget_RegDefaultUiConfigByDirPath("/home/rmbl/my_widget_config");
        if (widgetStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_ERROR("Widget config register failed, error code: %d", widgetStat);
        }
        static const T_DjiWidgetHandlerListItem s_widgetHandlerList[] = {
            {1, DJI_WIDGET_TYPE_SWITCH, OnWidgetSetValue, OnWidgetGetValue, NULL},
            {2, DJI_WIDGET_TYPE_SWITCH, OnWidgetSetValue, OnWidgetGetValue, NULL},
            {3, DJI_WIDGET_TYPE_SWITCH, OnWidgetSetValue, OnWidgetGetValue, NULL},
            {4, DJI_WIDGET_TYPE_SWITCH, OnWidgetSetValue, OnWidgetGetValue, NULL},
            {5, DJI_WIDGET_TYPE_SWITCH, OnWidgetSetValue, OnWidgetGetValue, NULL}
        };
        widgetStat = DjiWidget_RegHandlerList(s_widgetHandlerList,
                        sizeof(s_widgetHandlerList) / sizeof(s_widgetHandlerList[0]));
        if (widgetStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_ERROR("Widget handler register failed, error code: %d", widgetStat);
        }
        DjiLowSpeedDataChannel_RegRecvDataCallback(DJI_CHANNEL_ADDRESS_MASTER_RC_APP,
                                                   (DjiLowSpeedDataChannelRecvDataCallback)OnTextInputReceived);
        DjiTest_WidgetLogAppend("TriSonica Mini App initialized");
	    
    }
    
    pthread_t tri_th,fus_th,log_th;
    tri_reader_start(&tri_th);
    fusion_start(&fus_th);
    log_writer_start(&log_th);
    
    if(simulate_fc) {
		atomic_store_explicit(&logging_active, true, memory_order_relaxed);
		sleep(30);
		atomic_store_explicit(&logging_active, false, memory_order_relaxed);
		atomic_store_explicit(&exit_script_and_shutdown, true, memory_order_relaxed);
	}
    
	while(!atomic_load_explicit(&exit_script_and_shutdown, memory_order_relaxed)){
        DjiPlatform_GetOsalHandler()->TaskSleepMs(100);
    }
    
    if(!simulate_fc) {
        USER_LOG_INFO("--> Step 5: Deinit fc subscription module");
        djiStat = DjiFcSubscription_DeInit();
        if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_ERROR("Deinit fc subscription error.");
            return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
        }
        s_userFcSubscriptionDataShow = false;
        USER_LOG_INFO("Fc subscription sample end");    
    }
    
	if(!simulate_fc) {
	    system("sleep 5; sudo shutdown -h now");
	}
    exit(0);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}


T_DjiReturnCode DjiTest_FcSubscriptionDataShowTrigger(void)
{
    s_userFcSubscriptionDataShow = !s_userFcSubscriptionDataShow;

    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode DjiTest_FcSubscriptionGetTotalSatelliteNumber(uint8_t *number)
{
    *number = s_totalSatelliteNumberUsed;

    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}


/* Private functions definition-----------------------------------------------*/
#ifndef __CC_ARM
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#pragma GCC diagnostic ignored "-Wreturn-type"
#endif

static void *UserFcSubscription_Task(void *arg)
{
    T_DjiReturnCode djiStat;
    T_DjiFcSubscriptionVelocity velocity = {0};
    T_DjiDataTimestamp timestamp = {0};
    T_DjiFcSubscriptionGpsPosition gpsPosition = {0};
    T_DjiFcSubscriptionGpsDetails gpsDetails = {0};
    T_DjiOsalHandler *osalHandler = NULL;

    USER_UTIL_UNUSED(arg);
    osalHandler = DjiPlatform_GetOsalHandler();

    while (1) {
        osalHandler->TaskSleepMs(1000 / FC_SUBSCRIPTION_TASK_FREQ);

        djiStat = GetTopicLatestOrSimulate(DJI_FC_SUBSCRIPTION_TOPIC_VELOCITY,
                                                          (uint8_t *) &velocity,
                                                          sizeof(T_DjiFcSubscriptionVelocity),
                                                          &timestamp);
        if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_ERROR("get value of topic velocity error.");
        }

        if (s_userFcSubscriptionDataShow == true) {
            USER_LOG_INFO("velocity: x %f y %f z %f, healthFlag %d.", velocity.data.x, velocity.data.y,
                          velocity.data.z, velocity.health);
        }

        djiStat = GetTopicLatestOrSimulate(DJI_FC_SUBSCRIPTION_TOPIC_GPS_POSITION,
                                                          (uint8_t *) &gpsPosition,
                                                          sizeof(T_DjiFcSubscriptionGpsPosition),
                                                          &timestamp);
        if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_ERROR("get value of topic gps position error.");
        }

        if (s_userFcSubscriptionDataShow == true) {
            USER_LOG_INFO("gps position: x %d y %d z %d.", gpsPosition.x, gpsPosition.y, gpsPosition.z);
        }

        djiStat = GetTopicLatestOrSimulate(DJI_FC_SUBSCRIPTION_TOPIC_GPS_DETAILS,
                                                          (uint8_t *) &gpsDetails,
                                                          sizeof(T_DjiFcSubscriptionGpsDetails),
                                                          &timestamp);
        if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_ERROR("get value of topic gps details error.");
        }

        if (s_userFcSubscriptionDataShow == true) {
            USER_LOG_INFO("gps total satellite number used: %d %d %d.",
                          gpsDetails.gpsSatelliteNumberUsed,
                          gpsDetails.glonassSatelliteNumberUsed,
                          gpsDetails.totalSatelliteNumberUsed);
            s_totalSatelliteNumberUsed = gpsDetails.totalSatelliteNumberUsed;
        }
    }
}

#ifndef __CC_ARM
#pragma GCC diagnostic pop
#endif


static T_DjiReturnCode DjiTest_FcSubscriptionReceiveQuaternionCallback(const uint8_t *data, uint16_t dataSize,
                                                                       const T_DjiDataTimestamp *timestamp)
{
    T_DjiFcSubscriptionQuaternion *quaternion = (T_DjiFcSubscriptionQuaternion *) data;
    dji_f64_t pitch, yaw, roll;

    USER_UTIL_UNUSED(dataSize);

    pitch = (dji_f64_t) asinf(-2 * quaternion->q1 * quaternion->q3 + 2 * quaternion->q0 * quaternion->q2) * 57.3;
    roll = (dji_f64_t) atan2f(2 * quaternion->q2 * quaternion->q3 + 2 * quaternion->q0 * quaternion->q1,
                             -2 * quaternion->q1 * quaternion->q1 - 2 * quaternion->q2 * quaternion->q2 + 1) * 57.3;
    yaw = (dji_f64_t) atan2f(2 * quaternion->q1 * quaternion->q2 + 2 * quaternion->q0 * quaternion->q3,
                             -2 * quaternion->q2 * quaternion->q2 - 2 * quaternion->q3 * quaternion->q3 + 1) *
          57.3;

    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

// Override the weak function so that log messages are sent to the floating window.
void DjiTest_WidgetLogAppend(const char *fmt, ...)
{
    char logBuffer[DJI_WIDGET_FLOATING_WINDOW_MSG_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(logBuffer, DJI_WIDGET_FLOATING_WINDOW_MSG_MAX_LEN, fmt, args);
    va_end(args);

    // Display the message in the Smart Controller's floating log window.
    T_DjiReturnCode djiStat = DjiWidgetFloatingWindow_ShowMessage(logBuffer);
    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Floating window show message error, stat = 0x%08llX", djiStat);
    }
}

/****************** (C) COPYRIGHT DJI Innovations *****END OF FILE****/
