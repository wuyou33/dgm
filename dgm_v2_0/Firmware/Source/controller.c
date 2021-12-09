/*
	Copyright 2021 codenocold 1107795287@qq.com
	Address : https://github.com/codenocold/dgm
	This file is part of the dgm firmware.
	The dgm firmware is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
	The dgm firmware is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "controller.h"
#include "encoder.h"
#include "usr_config.h"
#include "trapTraj.h"
#include "pwm_curr_fdbk.h"
#include "anticogging.h"
#include "util.h"

ControllerStruct Controller;

static float mPosSetPoint;
static float mVelSetPoint;
static float mCurrSetPoint;

static float vel_integrator;

static volatile bool input_pos_updated;

void CONTROLLER_move_to_pos(float goal_point)
{
	Controller.input_position = goal_point;
	input_pos_updated = true;
}

static void move_to_pos(float goal_point)
{
    TRAJ_plan(goal_point, mPosSetPoint, mVelSetPoint,
                                 UsrConfig.traj_vel,		// Velocity
                                 UsrConfig.traj_accel,		// Acceleration
                                 UsrConfig.traj_decel);		// Deceleration
	
    Traj.t = 0.0f;
    Traj.trajectory_done = false;
}

float CONTROLLER_get_integrator_current(void)
{
	return vel_integrator;
}

void CONTROLLER_reset(ControllerStruct *controller)
{
	vel_integrator = 0;
	
	controller->input_position = Encoder.pos_estimate_;
	controller->input_velocity = 0;
	controller->input_current = 0;
	
	mPosSetPoint = Encoder.pos_estimate_;
	mVelSetPoint = 0;
	mCurrSetPoint = 0;
	
	input_pos_updated = false;
	Traj.trajectory_done = true;
}

float CONTROLLER_loop(ControllerStruct *controller, int control_mode, float velocity, float position)
{
	float vel_des;
	float current_des;
	
    switch (control_mode) {
        case CONTROL_MODE_CURRENT:{
			// Current limiting
			mCurrSetPoint = CLAMP(controller->input_current, -UsrConfig.current_limit, UsrConfig.current_limit);
			current_des = mCurrSetPoint;
		}break;
		
		case CONTROL_MODE_CURRENT_RAMP:{
			// Current limiting
			float target_current = CLAMP(controller->input_current, -UsrConfig.current_limit, UsrConfig.current_limit);
			
			// Current ramp
			float max_step_size = fabs(DT * UsrConfig.current_ramp_rate);
            float full_step = target_current - mCurrSetPoint;
            float step = CLAMP(full_step, -max_step_size, max_step_size);
            mCurrSetPoint += step;
			
			current_des = mCurrSetPoint;
		}break;
		
		case CONTROL_MODE_VELOCITY:{
			// Velocity limiting
			mVelSetPoint = CLAMP(controller->input_velocity, -UsrConfig.vel_limit, UsrConfig.vel_limit);
			vel_des = mVelSetPoint;
		}break;
		
		case CONTROL_MODE_VELOCITY_RAMP:{
			// Velocity limiting
			float target_velocity = CLAMP(controller->input_velocity, -UsrConfig.vel_limit, UsrConfig.vel_limit);
			
			// Velocity ramp
			float max_step_size = fabs(DT * UsrConfig.vel_ramp_rate);
            float full_step = target_velocity - mVelSetPoint;
            float step = CLAMP(full_step, -max_step_size, max_step_size);
            mVelSetPoint += step;
            mCurrSetPoint = (step / DT) * UsrConfig.inertia;
			
			vel_des = mVelSetPoint;
		}break;
		
		case CONTROL_MODE_POSITION:{
			mPosSetPoint = controller->input_position;
		}break;
		
		case CONTROL_MODE_POSITION_TRAP:{
			if(input_pos_updated){
                move_to_pos(controller->input_position);
                input_pos_updated = false;
            }
			
            // Avoid updating uninitialized trajectory
            if (Traj.trajectory_done){
                break;
			}
            
            if (Traj.t > Traj.Tf_) {
                mPosSetPoint = controller->input_position;
                mVelSetPoint = 0.0f;
                mCurrSetPoint = 0.0f;
                Traj.trajectory_done = true;
            } else {
				TRAJ_eval(Traj.t);
				mPosSetPoint = Traj.Y;
				mVelSetPoint = Traj.Yd;
				mCurrSetPoint = Traj.Ydd * UsrConfig.inertia;
				if(fabs(mPosSetPoint - Encoder.pos_estimate_) < 1.0f){
					Traj.t += DT;
				}
            }
		}break;
		
        default:
			break;
    }
	
	// Position
	if(UsrConfig.control_mode >= CONTROL_MODE_POSITION){
		float pos_err = mPosSetPoint - position;
		vel_des = mVelSetPoint + UsrConfig.pos_gain * pos_err;
		
		// Velocity limiting
		vel_des = CLAMP(vel_des, -UsrConfig.vel_limit, UsrConfig.vel_limit);
	}
	
	// Velocity
	float v_err;
	if(UsrConfig.control_mode >= CONTROL_MODE_VELOCITY){
		// Velocity PI ctrl
		v_err = vel_des - velocity;
		current_des = mCurrSetPoint + UsrConfig.vel_gain * v_err + vel_integrator;
	}
	
	// Anticogging
	if(UsrConfig.anticogging_enable && AnticoggingValid){
		current_des += pCoggingMap->map[(COGGING_MAP_NUM-1)*Encoder.cnt/ENCODER_CPR];
	}
	
	// Current limit & Integral saturation
	if(current_des < -UsrConfig.current_limit){
		current_des = -UsrConfig.current_limit;
	}else if(current_des > +UsrConfig.current_limit){
		current_des = +UsrConfig.current_limit;
	}else{
		vel_integrator += (UsrConfig.vel_integrator_gain * DT) * v_err;
	}
	
	return current_des;
}
