/*
 * Software License Agreement (BSD License)
 *
 *  [License text remains unchanged]
 */

/*
 *  @file AllegroHandDrv.cpp
 *  @brief Allegro Hand Driver
 *
 *  [File header remains unchanged]
 */

#include <iostream>
#include <math.h>
#include <stdio.h>
#include <string>
#include "ros/ros.h"
#include "candrv/candrv.h"
#include "allegro_hand_driver/AllegroHandDrv.h"

using namespace std;

#define MAX_DOF 16

#define PWM_LIMIT_ROLL 250.0*1.5
#define PWM_LIMIT_NEAR 450.0*1.5
#define PWM_LIMIT_MIDDLE 300.0*1.5
#define PWM_LIMIT_FAR 190.0*1.5

#define PWM_LIMIT_THUMB_ROLL 350.0*1.5
#define PWM_LIMIT_THUMB_NEAR 270.0*1.5
#define PWM_LIMIT_THUMB_MIDDLE 180.0*1.5
#define PWM_LIMIT_THUMB_FAR 180.0*1.5

#define PWM_LIMIT_GLOBAL_8V 800.0 // maximum: 1200
#define PWM_LIMIT_GLOBAL_24V 500.0
#define PWM_LIMIT_GLOBAL_12V 1200.0


namespace allegro
{

AllegroHandDrv::AllegroHandDrv()
    : _can_handle(0)
    , _curr_position_get(0)
    , _emergency_stop(false)
{
    ROS_INFO("AllegroHandDrv instance is constructed.");
}

AllegroHandDrv::~AllegroHandDrv()
{
    if (_can_handle != 0) {
        ROS_INFO("CAN: System Off");
        CANAPI::command_set_period(_can_handle, 0);
        usleep(10000);
        ROS_INFO("CAN: Close CAN channel");
        CANAPI::command_can_close(_can_handle);
    }
}

// trim from end. see http://stackoverflow.com/a/217605/256798
static inline std::string &rtrim(std::string &s)
{
    s.erase(std::find_if(
        s.rbegin(), s.rend(),
        std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
    return s;
}

bool AllegroHandDrv::init(int mode)
{
    string CAN_CH;
    ros::param::get("~comm/CAN_CH", CAN_CH);
    rtrim(CAN_CH);  // Ensure the ROS parameter has no trailing whitespace.

    if (CAN_CH.empty()) {
        ROS_ERROR("Invalid (empty) CAN channel, cannot proceed. Check PCAN comms.");
        return false;
    }

    if (CANAPI::command_can_open_with_name(_can_handle, CAN_CH.c_str())) {
        _can_handle = 0;
        return false;
    }

    ROS_INFO("CAN: Flush CAN receive buffer");
    CANAPI::command_can_flush(_can_handle);
    usleep(100);

    ROS_INFO("CAN: System Off");
    CANAPI::command_servo_off(_can_handle);
    usleep(100);

    ROS_INFO("CAN: Request Hand Information");
    CANAPI::request_hand_information(_can_handle);
    usleep(100);

    ROS_INFO("CAN: Request Hand Serial");
    CANAPI::request_hand_serial(_can_handle);
    usleep(100);

    ROS_INFO("CAN: Setting loop period(:= 3ms) and initialize system");
    short comm_period[3] = {3, 0, 0}; // millisecond {position, imu, temperature}
    CANAPI::command_set_period(_can_handle, comm_period);

    ROS_INFO("CAN: System ON");
    CANAPI::command_servo_on(_can_handle);
    usleep(100);

    ROS_INFO("CAN: Communicating");

    return true;
}

int AllegroHandDrv::readCANFrames()
{
    if (_emergency_stop)
        return -1;

    _readDevices();
    //usleep(10);

    return 0;
}

int AllegroHandDrv::writeJointTorque()
{
    _writeDevices();

    if (_emergency_stop) {
        ROS_ERROR("Emergency stop in writeJointTorque()");
        return -1;
    }

    return 0;
}

bool AllegroHandDrv::isJointInfoReady()
{
    return (_curr_position_get == (0x01 | 0x02 | 0x04 | 0x08));
}

void AllegroHandDrv::resetJointInfoReady()
{
    _curr_position_get = 0;
}

void AllegroHandDrv::setTorque(double *torque)
{
    for (int findex = 0; findex < 4; findex++) {
        _desired_torque[4*findex+0] = torque[4*findex+0];
        _desired_torque[4*findex+1] = torque[4*findex+1];
        _desired_torque[4*findex+2] = torque[4*findex+2];
        _desired_torque[4*findex+3] = torque[4*findex+3];
    }
}

void AllegroHandDrv::getJointInfo(double *position)
{
    for (int i = 0; i < DOF_JOINTS; i++) {
        position[i] = _curr_position[i];
    }
}

void AllegroHandDrv::_readDevices()
{
    int err;
    int id;
    int len;
    unsigned char data[8];

    err = CANAPI::can_read_message(_can_handle, &id, &len, data, FALSE, 0);
    while (!err) {
        _parseMessage(id, len, data);
        err = CANAPI::can_read_message(_can_handle, &id, &len, data, FALSE, 0);
    }
    //ROS_ERROR("can_read_message returns %d.", err); // PCAN_ERROR_QRCVEMPTY(32) from Peak CAN means "Receive queue is empty". It is not an error.
}

void AllegroHandDrv::_writeDevices()
{
    double pwmDouble[DOF_JOINTS];
    short pwm[DOF_JOINTS];

    if (!isJointInfoReady())
        return;

    // convert to torque to pwm
    for (int i = 0; i < DOF_JOINTS; i++) {
        pwmDouble[i] = _desired_torque[i] * _tau_cov_const;

        // limitation should be less than 800
        if (pwmDouble[i] > _pwm_max[i]) {
            pwmDouble[i] = _pwm_max[i];
        }
        else if (pwmDouble[i] < -_pwm_max[i]) {
            pwmDouble[i] = -_pwm_max[i];
        }

        pwm[i] = (short) pwmDouble[i];
    }

    for (int findex = 0; findex < 4; findex++) {
        CANAPI::command_set_torque(_can_handle, findex, &pwm[findex*4]);
        //ROS_INFO("write torque %d: %d %d %d %d", findex, pwm[findex*4+0], pwm[findex*4+1], pwm[findex*4+2], pwm[findex*4+3]);
    }
}

void AllegroHandDrv::_parseMessage(int id, int len, unsigned char* data)
{
    int tmppos[4];
    int lIndexBase;
    int i;

    //v4
    switch (id)
    {
        case ID_RTR_HAND_INFO:
        {
            // Replaced printf with ROS_INFO and appropriate format specifiers
            ROS_INFO(">CAN(%p): AllegroHand hardware version: 0x%02x%02x",
                     (void*)_can_handle, data[1], data[0]);
            ROS_INFO("                      firmware version: 0x%02x%02x", data[3], data[2]);
            ROS_INFO("                      hardware type: %d(%s)", data[4], (data[4] == 0 ? "right" : "left"));
            ROS_INFO("                      temperature: %d (celsius)", data[5]);
            ROS_INFO("                      status: 0x%02x", data[6]);
            ROS_INFO("                      servo status: %s", (data[6] & 0x01 ? "ON" : "OFF"));
            ROS_INFO("                      high temperature fault: %s", (data[6] & 0x02 ? "ON" : "OFF"));
            ROS_INFO("                      internal communication fault: %s", (data[6] & 0x04 ? "ON" : "OFF"));

            _hand_version = data[1];
            _tau_cov_const = 1200.0;
            _input_voltage = 12.0;
            _pwm_max_global = PWM_LIMIT_GLOBAL_12V;

            _pwm_max[eJOINTNAME_INDEX_0] = min(_pwm_max_global, PWM_LIMIT_ROLL);
            _pwm_max[eJOINTNAME_INDEX_1] = min(_pwm_max_global, PWM_LIMIT_NEAR);
            _pwm_max[eJOINTNAME_INDEX_2] = min(_pwm_max_global, PWM_LIMIT_MIDDLE);
            _pwm_max[eJOINTNAME_INDEX_3] = min(_pwm_max_global, PWM_LIMIT_FAR);

            _pwm_max[eJOINTNAME_MIDDLE_0] = min(_pwm_max_global, PWM_LIMIT_ROLL);
            _pwm_max[eJOINTNAME_MIDDLE_1] = min(_pwm_max_global, PWM_LIMIT_NEAR);
            _pwm_max[eJOINTNAME_MIDDLE_2] = min(_pwm_max_global, PWM_LIMIT_MIDDLE);
            _pwm_max[eJOINTNAME_MIDDLE_3] = min(_pwm_max_global, PWM_LIMIT_FAR);

            _pwm_max[eJOINTNAME_PINKY_0] = min(_pwm_max_global, PWM_LIMIT_ROLL);
            _pwm_max[eJOINTNAME_PINKY_1] = min(_pwm_max_global, PWM_LIMIT_NEAR);
            _pwm_max[eJOINTNAME_PINKY_2] = min(_pwm_max_global, PWM_LIMIT_MIDDLE);
            _pwm_max[eJOINTNAME_PINKY_3] = min(_pwm_max_global, PWM_LIMIT_FAR);

            _pwm_max[eJOINTNAME_THUMB_0] = min(_pwm_max_global, PWM_LIMIT_THUMB_ROLL);
            _pwm_max[eJOINTNAME_THUMB_1] = min(_pwm_max_global, PWM_LIMIT_THUMB_NEAR);
            _pwm_max[eJOINTNAME_THUMB_2] = min(_pwm_max_global, PWM_LIMIT_THUMB_MIDDLE);
            _pwm_max[eJOINTNAME_THUMB_3] = min(_pwm_max_global, PWM_LIMIT_THUMB_FAR);
        }
            break;
        case ID_RTR_SERIAL:
        {
            // Replaced printf with ROS_INFO
            ROS_INFO(">CAN(%p): AllegroHand serial number: SAH0%d0 %c%c%c%c%c%c%c%c",
                     (void*)_can_handle, /*HAND_VERSION*/4,
                     data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
        }
            break;
        case ID_RTR_FINGER_POSE_1:
        case ID_RTR_FINGER_POSE_2:
        case ID_RTR_FINGER_POSE_3:
        case ID_RTR_FINGER_POSE_4:
        {
            int findex = (id & 0x00000007);

            tmppos[0] = (short) (data[0] | (data[1] << 8));
            tmppos[1] = (short) (data[2] | (data[3] << 8));
            tmppos[2] = (short) (data[4] | (data[5] << 8));
            tmppos[3] = (short) (data[6] | (data[7] << 8));

            lIndexBase = findex * 4;

            _curr_position[lIndexBase+0] = (double)(tmppos[0]) * ( 333.3 / 65536.0 ) * ( M_PI/180.0);
            _curr_position[lIndexBase+1] = (double)(tmppos[1]) * ( 333.3 / 65536.0 ) * ( M_PI/180.0);
            _curr_position[lIndexBase+2] = (double)(tmppos[2]) * ( 333.3 / 65536.0 ) * ( M_PI/180.0);
            _curr_position[lIndexBase+3] = (double)(tmppos[3]) * ( 333.3 / 65536.0 ) * ( M_PI/180.0);

            _curr_position_get |= (0x01 << (findex));
        }
            break;
        case ID_RTR_IMU_DATA:
        {
            // Replaced printf with ROS_INFO
            ROS_INFO(">CAN(%p): AHRS Roll : 0x%02x%02x", (void*)_can_handle, data[0], data[1]);
            ROS_INFO("               Pitch: 0x%02x%02x", data[2], data[3]);
            ROS_INFO("               Yaw  : 0x%02x%02x", data[4], data[5]);
        }
            break;
        case ID_RTR_TEMPERATURE_1:
        case ID_RTR_TEMPERATURE_2:
        case ID_RTR_TEMPERATURE_3:
        case ID_RTR_TEMPERATURE_4:
        {
            int sindex = (id & 0x00000007);
            int celsius = (int)(data[0]      ) |
                          (int)(data[1] << 8 ) |
                          (int)(data[2] << 16) |
                          (int)(data[3] << 24);
            // Replaced printf with ROS_INFO
            ROS_INFO(">CAN(%p): Temperature[%d]: %d (celsius)", (void*)_can_handle, sindex, celsius);
        }
            break;
        default:
            ROS_WARN("unknown command %d, len %d", id, len);
            for(int nd=0; nd<len; nd++)
                printf("%d \n ", data[nd]);
            return;
    }
}

} // namespace allegro
