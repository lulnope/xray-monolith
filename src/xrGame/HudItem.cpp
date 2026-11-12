#include "stdafx.h"
#include "HudItem.h"
#include "physic_item.h"
#include "actor.h"
#include "actoreffector.h"
#include "Missile.h"
#include "xrmessages.h"
#include "level.h"
#include "inventory.h"
#include "../xrEngine/CameraBase.h"
#include "player_hud.h"
#include "../xrEngine/SkeletonMotions.h"

#include "../build_config_defines.h"
#include "ui_base.h"
#include "ui\UIScriptWnd.h"

#include "script_callback_ex.h"
#include "script_game_object.h"
#include "Flashlight.h"
#include "clsid_game.h"
#include "weaponpistol.h"
#include "HUDManager.h"

#include "script_attachment_manager.h"

ENGINE_API extern float psHUD_FOV_def;
int g_nearwall = NW_FOV;
int g_nearwall_trace = NT_CAM;

// verdatim: 0 (default) = motion marks fire on real time since the animation started, which is what existing mods
// are tuned for; 1 = marks follow the animation, so they move with its playback speed.
BOOL scale_hud_motion_marks_by_speed = FALSE;

CHudItem::CHudItem()
{
	RenderHud(TRUE);
	EnableHudInertion(TRUE);
	AllowHudInertion(TRUE);
	m_bStopAtEndAnimIsRunning = false;
	m_current_motion_def = nullptr;
	m_started_rnd_anim_idx = u8(-1);

	m_fPreviousMotionTime = 0.f;
	m_fCurrentMotionTime = 0.f;
	m_fCurrentMotionLength = 0.f;
	m_fCurrentMotionSpeed = 0.f;
	m_fCurrentMotionEnd = 0.f;
	m_dwMotionLastUpdateTm = 0;
	m_motion_serial = 0;

	m_fLR_CameraFactor = 0.f;
	m_fLR_MovingFactor = 0.f;
	m_fLR_InertiaFactor = 0.f;
	m_fUD_InertiaFactor = 0.f;

	m_nearwall_factor = 0.f;
	m_lastState = eHidden;

	script_ui = nullptr;
	script_ui_funct = nullptr;
	script_ui_bone = nullptr;
	script_ui_matrix.identity();

	m_instance_holder_hud = nullptr;
}

DLL_Pure* CHudItem::_construct()
{
	m_object = smart_cast<CPhysicItem*>(this);
	VERIFY(m_object);

	m_item = smart_cast<CInventoryItem*>(this);
	VERIFY(m_item);

	return (m_object);
}

CHudItem::~CHudItem()
{
	DeleteHudItemData();
}

void CHudItem::DeleteHudItemData()
{
	xr_delete(m_attachable);
	m_attachable = nullptr;
}

void CHudItem::Load(LPCSTR section)
{
	hud_sect = pSettings->r_string(section, "hud");
	m_animation_slot = pSettings->r_u32(section, "animation_slot");

	m_sounds.LoadSound(section, "snd_bore", "sndBore", true);

	m_hud_fov_add_mod = READ_IF_EXISTS(pSettings, r_float, section, "hud_fov_addition_modifier", 0.f);
	m_nearwall_dist_min = READ_IF_EXISTS(pSettings, r_float, section, "nearwall_dist_min", .2f);
	m_nearwall_dist_max = READ_IF_EXISTS(pSettings, r_float, section, "nearwall_dist_max", 1.f);
	m_nearwall_target_hud_fov = READ_IF_EXISTS(pSettings, r_float, section, "nearwall_target_hud_fov", 0.27f);
	m_nearwall_speed_mod = READ_IF_EXISTS(pSettings, r_float, section, "nearwall_speed_mod", 10.f);
	m_base_fov = READ_IF_EXISTS(pSettings, r_float, section, "hud_fov", 0.f);

	// Rezy - Custom Script 3D UI
	script_ui_funct = READ_IF_EXISTS(pSettings, r_string, section, "custom_ui_func", nullptr);

	if (script_ui_funct)
	{
		script_ui_bone = READ_IF_EXISTS(pSettings, r_string, section, "custom_ui_bone", "wpn_body");
		script_ui_offset[0] = READ_IF_EXISTS(pSettings, r_fvector3, section, "custom_ui_pos", Fvector().set(0.f, 0.f, 0.f));
		script_ui_offset[1] = READ_IF_EXISTS(pSettings, r_fvector3, section, "custom_ui_rot", Fvector().set(0.f, 0.f, 0.f));

		script_ui_offset[1].mul(PI / 180.f);
		script_ui_matrix.setHPB(script_ui_offset[1].x, script_ui_offset[1].y, script_ui_offset[1].z);
		script_ui_matrix.translate_over(script_ui_offset[0]);
	}
}

void CHudItem::PlaySound(LPCSTR alias, const Fvector& position)
{
	m_sounds.PlaySound(alias, position, object().H_Root(), !!GetHUDmode());
}

//Alundaio: Play at index
void CHudItem::PlaySound(LPCSTR alias, const Fvector& position, u8 index)
{
	m_sounds.PlaySound(alias, position, object().H_Root(), !!GetHUDmode(), false, index);
}

//-Alundaio

void CHudItem::renderable_Render(IDSGraphManager* DM)
{
	UpdateXForm();
	BOOL _hud_render = DM->get_HUD() && GetHUDmode();

	if (_hud_render && !IsHidden())
	{
	}
	else
	{
		if (!object().H_Parent() || (!_hud_render && !IsHidden()))
		{
			on_renderable_Render(DM);
			debug_draw_firedeps();
		}
		else if (object().H_Parent())
		{
			CInventoryOwner* owner = smart_cast<CInventoryOwner*>(object().H_Parent());
            if (owner)
            {
                CInventoryItem* self = smart_cast<CInventoryItem*>(this);
                if (owner->attached(self))
                    on_renderable_Render(DM);
            }			
		}
	}
}

void CHudItem::SwitchState(u32 S)
{
	if (OnClient())
		return;

	SetNextState(S);

	if (object().Local() && !object().getDestroy())
	{
		// !!! Just single entry for given state !!!
		NET_Packet P;
		object().u_EventGen(P, GE_WPN_STATE_CHANGE, object().ID());
		P.w_u8(u8(S));
		object().u_EventSend(P);
	}
}

void CHudItem::OnEvent(NET_Packet& P, u16 type)
{
	switch (type)
	{
	case GE_WPN_STATE_CHANGE:
		{
			u8 S;
			P.r_u8(S);
			OnStateSwitch(u32(S), GetState());
		}
		break;
	}
}

void CHudItem::OnStateSwitch(u32 S, u32 oldState)
{
	m_lastState = oldState;
	SetState(S);

	if (object().Remote())
		SetNextState(S);

	switch (S)
	{
	case eBore:
		SetPending(FALSE);

		if (TryPlayAnimBore())
		{
			if (IsAttachedToHUD())
			{
				Fvector P = HudItemData()->m_item_transform.c;
				m_sounds.PlaySound("sndBore", P, object().H_Root(), !!GetHUDmode(), false, m_started_rnd_anim_idx);
			}
		}
		else
			SwitchState(eIdle);

		break;
	case eHidden:
		if (IsAttachedToHUD())
			g_player_hud->detach_item(this);
		break;
	}

	g_player_hud->updateMovementLayerState();

	::luabind::functor<void> funct;
	if (ai().script_engine().functor("_G.CHudItem__OnStateSwitch", funct))
	{
		funct(smart_cast<CGameObject*>(this)->lua_game_object(), S, oldState);
	}
}

void CHudItem::OnAnimationEnd(u32 state)
{
	CActor* A = smart_cast<CActor*>(object().H_Parent());
	if (A)
		A->callback(GameObject::eActorHudAnimationEnd)(smart_cast<CGameObject*>(this)->lua_game_object(),
		                                               this->hud_sect.c_str(), this->m_current_motion.c_str(), state,
		                                               this->animation_slot());

	switch (state)
	{
	case eBore:
		{
			SwitchState(eIdle);
		}
		break;
	}
}

bool CHudItem::TryPlayAnimBore() 
{
	if (HudAnimationExist("anm_bore"))
	{
		PlayHUDMotion("anm_bore", TRUE, this, GetState());
		return true;
	}

	return false;
}

bool CHudItem::ActivateItem()
{
	OnActiveItem();
	return true;
}

void CHudItem::DeactivateItem()
{
	OnHiddenItem();
}

void CHudItem::OnMoveToRuck(const SInvItemPlace& prev)
{
	SwitchState(eHidden);
}

void CHudItem::SendDeactivateItem()
{
	SendHiddenItem();
}

void CHudItem::SendHiddenItem()
{
	if (!object().getDestroy())
	{
		NET_Packet P;
		object().u_EventGen(P, GE_WPN_STATE_CHANGE, object().ID());
		P.w_u8(u8(eHiding));
		object().u_EventSend(P, net_flags(TRUE, TRUE, FALSE, TRUE));
	}
}

void CHudItem::UpdateHudAdditional(Fmatrix& trans)
{
	CActor* pActor = smart_cast<CActor*>(object().H_Parent());
	if (!pActor)
		return;

	attachable_hud_item* hi = HudItemData();

	if (!g_player_hud->inertion_allowed())
		return;

	static float fAvgTimeDelta = Device.fTimeDelta;
	fAvgTimeDelta = _inertion(fAvgTimeDelta, Device.fTimeDelta, 0.8f);

	float fYMag = pActor->fFPCamYawMagnitude;
	float fPMag = pActor->fFPCamPitchMagnitude;

	float fStrafeMaxTime = hi->m_measures.m_strafe_offset[2][0].y;
	// Макс. время в секундах, за которое мы наклонимся из центрального положения
	if (fStrafeMaxTime <= EPS)
		fStrafeMaxTime = 0.01f;

	float fStepPerUpd = fAvgTimeDelta / fStrafeMaxTime; // Величина изменение фактора поворота

														// Добавляем боковой наклон от движения камеры
	float fCamReturnSpeedMod = 1.5f;
	// Восколько ускоряем нормализацию наклона, полученного от движения камеры (только от бедра)

	// Высчитываем минимальную скорость поворота камеры для начала инерции
	float fStrafeMinAngle = hi->m_measures.m_strafe_offset[3][0].y;

	// Высчитываем мксимальный наклон от поворота камеры
	float fCamLimitBlend = hi->m_measures.m_strafe_offset[3][0].x;

	// Считаем стрейф от поворота камеры
	if (abs(fYMag) > (m_fLR_CameraFactor == 0.0f ? fStrafeMinAngle : 0.0f))
	{
		//--> Камера крутится по оси Y
		m_fLR_CameraFactor -= (fYMag * fAvgTimeDelta * 0.75f);
		clamp(m_fLR_CameraFactor, -fCamLimitBlend, fCamLimitBlend);
	}
	else
	{
		//--> Камера не поворачивается - убираем наклон
		if (m_fLR_CameraFactor < 0.0f)
		{
			m_fLR_CameraFactor += fStepPerUpd * fCamReturnSpeedMod;
			clamp(m_fLR_CameraFactor, -fCamLimitBlend, 0.0f);
		}
		else
		{
			m_fLR_CameraFactor -= fStepPerUpd * fCamReturnSpeedMod;
			clamp(m_fLR_CameraFactor, 0.0f, fCamLimitBlend);
		}
	}

	// Добавляем боковой наклон от ходьбы вбок
	float fChangeDirSpeedMod = 3;
	// Восколько быстро меняем направление направление наклона, если оно в другую сторону от текущего
	u32 iMovingState = pActor->MovingState();
	if ((iMovingState & mcLStrafe) != 0)
	{
		// Движемся влево
		float fVal = (m_fLR_MovingFactor > 0.f ? fStepPerUpd * fChangeDirSpeedMod : fStepPerUpd);
		m_fLR_MovingFactor -= fVal;
	}
	else if ((iMovingState & mcRStrafe) != 0)
	{
		// Движемся вправо
		float fVal = (m_fLR_MovingFactor < 0.f ? fStepPerUpd * fChangeDirSpeedMod : fStepPerUpd);
		m_fLR_MovingFactor += fVal;
	}
	else
	{
		// Двигаемся в любом другом направлении - плавно убираем наклон
		if (m_fLR_MovingFactor < 0.0f)
		{
			m_fLR_MovingFactor += fStepPerUpd;
			clamp(m_fLR_MovingFactor, -1.0f, 0.0f);
		}
		else
		{
			m_fLR_MovingFactor -= fStepPerUpd;
			clamp(m_fLR_MovingFactor, 0.0f, 1.0f);
		}
	}
	clamp(m_fLR_MovingFactor, -1.0f, 1.0f); // Фактор боковой ходьбы не должен превышать эти лимиты

											// Вычисляем и нормализируем итоговый фактор наклона
	float fLR_Factor = m_fLR_MovingFactor;

	// No cam strafe inertia while in freelook mode
	if (pActor->cam_freelook == eflDisabled)
		fLR_Factor += m_fLR_CameraFactor;

	clamp(fLR_Factor, -1.0f, 1.0f); // Фактор боковой ходьбы не должен превышать эти лимиты

	Fvector curr_offs, curr_rot;
	Fmatrix hud_rotation;
	Fmatrix hud_rotation_y;

	if ((hi->m_measures.m_strafe_offset[2][0].x != 0.0f))
	{
		// Смещение позиции худа в стрейфе
		curr_offs = hi->m_measures.m_strafe_offset[0][0]; // pos
		curr_offs.mul(fLR_Factor); // Умножаем на фактор стрейфа

								   // Поворот худа в стрейфе
		curr_rot = hi->m_measures.m_strafe_offset[1][0]; // rot
		curr_rot.mul(-PI / 180.f); // Преобразуем углы в радианы
		curr_rot.mul(fLR_Factor); // Умножаем на фактор стрейфа

		hud_rotation.identity();
		hud_rotation.rotateX(curr_rot.x);

		hud_rotation_y.identity();
		hud_rotation_y.rotateY(curr_rot.y);
		hud_rotation.mulA_43(hud_rotation_y);

		hud_rotation_y.identity();
		hud_rotation_y.rotateZ(curr_rot.z);
		hud_rotation.mulA_43(hud_rotation_y);

		hud_rotation.translate_over(curr_offs);
		trans.mulB_43(hud_rotation);
	}

	//============= Инерция оружия =============//
	// Параметры инерции
	float fInertiaSpeedMod = hi->m_measures.m_inertion_params.m_tendto_speed;

	float fInertiaReturnSpeedMod = hi->m_measures.m_inertion_params.m_tendto_ret_speed;

	float fInertiaMinAngle = hi->m_measures.m_inertion_params.m_min_angle;

	Fvector4 vIOffsets; // x = L, y = R, z = U, w = D
	vIOffsets.x = hi->m_measures.m_inertion_params.m_offset_LRUD.x;
	vIOffsets.y = hi->m_measures.m_inertion_params.m_offset_LRUD.y;
	vIOffsets.z = hi->m_measures.m_inertion_params.m_offset_LRUD.z;
	vIOffsets.w = hi->m_measures.m_inertion_params.m_offset_LRUD.w;

	// Высчитываем инерцию из поворотов камеры
	bool bIsInertionPresent = m_fLR_InertiaFactor != 0.0f || m_fUD_InertiaFactor != 0.0f;
	if (abs(fYMag) > fInertiaMinAngle || bIsInertionPresent)
	{
		float fSpeed = fInertiaSpeedMod;
		if (fYMag > 0.0f && m_fLR_InertiaFactor > 0.0f ||
			fYMag < 0.0f && m_fLR_InertiaFactor < 0.0f)
		{
			fSpeed *= 2.f; //--> Ускоряем инерцию при движении в противоположную сторону
		}

		m_fLR_InertiaFactor -= (fYMag * fAvgTimeDelta * fSpeed); // Горизонталь (м.б. > |1.0|)
	}

	if (abs(fPMag) > fInertiaMinAngle || bIsInertionPresent)
	{
		float fSpeed = fInertiaSpeedMod;
		if (fPMag > 0.0f && m_fUD_InertiaFactor > 0.0f ||
			fPMag < 0.0f && m_fUD_InertiaFactor < 0.0f)
		{
			fSpeed *= 2.f; //--> Ускоряем инерцию при движении в противоположную сторону
		}

		m_fUD_InertiaFactor -= (fPMag * fAvgTimeDelta * fSpeed); // Вертикаль (м.б. > |1.0|)
	}

	clamp(m_fLR_InertiaFactor, -1.0f, 1.0f);
	clamp(m_fUD_InertiaFactor, -1.0f, 1.0f);

	// Плавное затухание инерции (основное, но без линейной никогда не опустит инерцию до полного 0.0f)
	m_fLR_InertiaFactor *= clampr(1.f - fAvgTimeDelta * fInertiaReturnSpeedMod, 0.0f, 1.0f);
	m_fUD_InertiaFactor *= clampr(1.f - fAvgTimeDelta * fInertiaReturnSpeedMod, 0.0f, 1.0f);

	// Минимальное линейное затухание инерции при покое (горизонталь)
	if (fYMag == 0.0f)
	{
		float fRetSpeedMod = (fYMag == 0.0f ? 1.0f : 0.75f) * (fInertiaReturnSpeedMod * 0.075f);
		if (m_fLR_InertiaFactor < 0.0f)
		{
			m_fLR_InertiaFactor += fAvgTimeDelta * fRetSpeedMod;
			clamp(m_fLR_InertiaFactor, -1.0f, 0.0f);
		}
		else
		{
			m_fLR_InertiaFactor -= fAvgTimeDelta * fRetSpeedMod;
			clamp(m_fLR_InertiaFactor, 0.0f, 1.0f);
		}
	}

	// Минимальное линейное затухание инерции при покое (вертикаль)
	if (fPMag == 0.0f)
	{
		float fRetSpeedMod = (fPMag == 0.0f ? 1.0f : 0.75f) * (fInertiaReturnSpeedMod * 0.075f);
		if (m_fUD_InertiaFactor < 0.0f)
		{
			m_fUD_InertiaFactor += fAvgTimeDelta * fRetSpeedMod;
			clamp(m_fUD_InertiaFactor, -1.0f, 0.0f);
		}
		else
		{
			m_fUD_InertiaFactor -= fAvgTimeDelta * fRetSpeedMod;
			clamp(m_fUD_InertiaFactor, 0.0f, 1.0f);
		}
	}

	// Применяем инерцию к худу
	float fLR_lim = (m_fLR_InertiaFactor < 0.0f ? vIOffsets.x : vIOffsets.y);
	float fUD_lim = (m_fUD_InertiaFactor < 0.0f ? vIOffsets.z : vIOffsets.w);

	curr_offs = { fLR_lim * -1.f * m_fLR_InertiaFactor, fUD_lim * m_fUD_InertiaFactor, 0.0f };

	hud_rotation.identity();
	hud_rotation.translate_over(curr_offs);
	trans.mulB_43(hud_rotation);
}

float CHudItem::GetNearWallRange()
{
	return m_nearwall_dist_max - m_nearwall_dist_min;
}

static float lerp(float a, float b, float t)
{
	clamp(t, 0.f, 1.f);
	return a * (1 - t) + b * t;
}

float CHudItem::GetTargetHudFov()
{
	float target_fov = GetBaseHudFov();
	if (g_nearwall == NW_FOV)
		target_fov -= m_nearwall_target_hud_fov * m_nearwall_factor;
	return target_fov;
}

float CHudItem::GetTargetNearWallOffset()
{
	if (g_nearwall_trace == NT_CAM)
	{
		return m_nearwall_factor * GetNearWallRange() * GetBaseHudFov();
	}
	else if (g_nearwall_trace == NT_ITEM)
	{
		return m_nearwall_factor * GetBaseHudFov();
	}

	return 0.f;
}

void CHudItem::UpdateNearWall()
{
	if (g_nearwall && ParentIsActor() && Level().CurrentViewEntity() == object().H_Parent())
	{
		if (g_nearwall_trace == NT_CAM)
		{
			// Use the HUD trace, and lerp between min and max distances
			collide::rq_result& rq = HUD().GetRQ();

			float dist = rq.range;
			clamp(dist, m_nearwall_dist_min, m_nearwall_dist_max);
			m_nearwall_factor = 1 - ((dist - m_nearwall_dist_min) / GetNearWallRange());
		}
		else if (g_nearwall_trace == NT_ITEM)
		{
			// Take the item's trace range and invert it, as negative ranges encode penetration distance
			collide::rq_result& rq = GetRQ();
			m_nearwall_factor = -rq.range;
			clamp(m_nearwall_factor, 0.f, m_nearwall_factor);
		}
	}
}

// Same as END_EPS in CBlend::update_time: a stop-at-end blend freezes this long before the end of the motion,
// and a looping blend restarts there.
static const float HUD_MOTION_END_EPS = SAMPLE_SPF + EPS;

void CHudItem::UpdateCL()
{
	if (m_current_motion_def && !UpdateMotionClock())
		return; // the animation finished and OnAnimationEnd ran this frame

	if (script_ui)
		script_ui->Update();
}

// Advances the motion clock, fires motion marks and reports the end of stop-at-end animations.
// Returns false if OnAnimationEnd was called.
bool CHudItem::UpdateMotionClock()
{
	// Same time source as the skeleton (CKinematicsAnimated::UpdateTracks), so the clock follows what is on screen
	// and does not fall behind when this object is updated less often than every frame (NPC weapons).
	u32 elapsed_ms = Device.dwTimeGlobal - m_dwMotionLastUpdateTm;
	m_dwMotionLastUpdateTm = Device.dwTimeGlobal;
	if (IsAttachedToHUD() && elapsed_ms > 66)
		elapsed_ms = 66; // UpdateTracks limits its step the same way

	m_fPreviousMotionTime = m_fCurrentMotionTime;
	m_fCurrentMotionTime += float(elapsed_ms) / 1000.f * m_fCurrentMotionSpeed;

	const u32 motion_serial = m_motion_serial;

	if (!m_bStopAtEndAnimIsRunning)
	{
		// Looping blends restart at (length - END_EPS), see CBlend::update_time
		const float loop_length = m_fCurrentMotionLength - HUD_MOTION_END_EPS;
		if (loop_length > EPS_L && m_fCurrentMotionTime > loop_length)
		{
			// Rest of this cycle, including marks placed in the last frame
			if (!FireMotionMarks(m_fPreviousMotionTime, m_fCurrentMotionLength, motion_serial))
				return true;

			m_fPreviousMotionTime = 0.f;
			m_fCurrentMotionTime -= loop_length;
			if (m_fCurrentMotionTime > loop_length) // whole cycles were skipped, don't replay their marks
				m_fCurrentMotionTime = fmodf(m_fCurrentMotionTime, loop_length);
		}
	}

	if (!FireMotionMarks(m_fPreviousMotionTime, m_fCurrentMotionTime, motion_serial))
		return true;

	if (m_bStopAtEndAnimIsRunning && m_fCurrentMotionTime > m_fCurrentMotionEnd)
	{
		StopCurrentAnimWithoutCallback();
		OnAnimationEnd(m_startedMotionState);
		return false;
	}

	return true;
}

// Fires the marks of the current animation that start in [from, to).
// Returns false if a mark handler started or stopped an animation.
bool CHudItem::FireMotionMarks(float from, float to, u32 motion_serial)
{
	if (to <= from)
		return true;

	// from/to are animation time. Unless marks follow the playback speed, compare them on real time since the
	// animation (or loop cycle) started instead.
	if (!scale_hud_motion_marks_by_speed && m_fCurrentMotionSpeed > 0.f)
	{
		from /= m_fCurrentMotionSpeed;
		to /= m_fCurrentMotionSpeed;
	}

	// Engine gameplay handlers (reload, throw, knife hit, shotgun unjam, flashlight switch) have only ever reacted
	// to marks of stop-at-end animations. Marks of looping animations are reported to scripts only.
	const bool engine_handlers = m_bStopAtEndAnimIsRunning;

	// Indexed access: a handler may rewrite the marks (set_hand_motion_marks) while we iterate
	const xr_vector<motion_marks>& marks = m_current_motion_def->marks;
	for (size_t i = 0; i < marks.size(); ++i)
	{
		for (size_t j = 0; i < marks.size() && j < marks[i].get_intervals().size(); ++j)
		{
			const motion_marks::interval interval = marks[i].get_intervals()[j];
			if (interval.first < from || interval.first >= to)
				continue;

			if (engine_handlers)
				OnMotionMark(m_startedMotionState, marks[i], interval.first, interval.second);
			else
				CHudItem::OnMotionMark(m_startedMotionState, marks[i], interval.first, interval.second);

			if (motion_serial != m_motion_serial || !m_current_motion_def)
				return false;
		}
	}

	return true;
}

void CHudItem::OnMotionMark(u32 state, const motion_marks& M, float mark_start, float mark_end)
{
	::luabind::functor<bool> funct;
	if (ai().script_engine().functor("_G.CHudItem__OnMotionMark", funct))
		funct(state, *M.name, object().lua_game_object(), object().lua_game_object() ? object().lua_game_object()->Parent() : nullptr, mark_start, mark_end);
}

void CHudItem::OnH_A_Chield()
{
}

void CHudItem::OnH_B_Chield()
{
	StopCurrentAnimWithoutCallback();
}

void CHudItem::OnH_B_Independent(bool just_before_destroy)
{
	m_sounds.StopAllSounds();
	UpdateXForm();
	m_nearwall_factor = 0.f;

	// next code was commented
	/*
	if(IsAttachedToHUD() && !just_before_destroy)
	{
	object().XFORM().set( HudItemData()->m_item_transform );
	}

	if (IsAttachedToHUD())
	{
	g_player_hud->detach_item(this);
	Msg("---Detaching hud item [%s][%d]", this->HudSection().c_str(), this->object().ID());
	}*/
	//SetHudItemData			(NULL);
}

void CHudItem::OnH_A_Independent()
{
	if (IsAttachedToHUD())
		g_player_hud->detach_item(this);
	StopCurrentAnimWithoutCallback();
}

void CHudItem::on_b_hud_detach()
{
	m_sounds.StopAllSounds();
}

void CHudItem::on_outfit_changed()
{
	if (m_current_motion_def)
		PlayHUDMotion_noCB(m_current_motion, FALSE);
}

void CHudItem::on_a_hud_attach()
{
	if (script_ui_funct && nullptr == script_ui)
	{
		::luabind::functor<CUIDialogWndEx*> funct;
		if (ai().script_engine().functor(script_ui_funct, funct))
		{
			CUIDialogWndEx* ret = funct();
			CUIWindow* pScriptWnd = ret ? smart_cast<CUIWindow*>(ret) : (0);
			if (pScriptWnd)
				script_ui = pScriptWnd;
			else
				Msg("[%s]: Failed to load script UI [%s]!", object().cNameSect_str(), script_ui_funct);
		}
		else
			Msg("[%s]: Script UI functor [%s] does not exist!", object().cNameSect_str(), script_ui_funct);
	}
}

void CHudItem::render_item_3d_ui()
{
	if (script_ui)
	{
		Fmatrix LM;
		Fmatrix trans = HudItemData()->m_item_transform;
		u16 bid = HudItemData()->m_model->LL_BoneID(script_ui_bone);
		Fmatrix ui_bone = HudItemData()->m_model->LL_GetTransform(bid);
		LM.mul(trans, ui_bone);

		if (g_player_hud->m_adjust_mode)
		{
			Fmatrix script_ui_adjust_matrix;
			script_ui_adjust_matrix.identity();
			Fvector& pos = g_player_hud->m_adjust_ui_offset[0];
			Fvector& rot = g_player_hud->m_adjust_ui_offset[1];

			script_ui_adjust_matrix.setHPB(rot.x, rot.y, rot.z);
			script_ui_adjust_matrix.translate_over(pos);
			LM.mulB_43(script_ui_adjust_matrix);
		}
		else
			LM.mulB_43(script_ui_matrix);

		UIRender->CacheSetXformWorld(LM);
		script_ui->Draw();
	}
}

extern float g_end_modif;

u32 CHudItem::PlayHUDMotion(shared_str M, BOOL bMixIn, CHudItem* W, u32 state, float speed, float end, bool bMixIn2)
{
	if (IsAttachedToHUD())
	{
		::luabind::functor<::luabind::object> funct;
		if (ai().script_engine().functor("_G.CHudItem__PlayHUDMotion", funct))
		{
			
			::luabind::object table = ::luabind::newtable(ai().script_engine().lua());
			table["anm_name"] = *M;
			table["anm_mixin"] = !!bMixIn;
			table["anm_mixin2"] = bMixIn2;
			table["anm_state"] = state;
			table["anm_speed"] = speed;
			table["anm_end"] = end;

			::luabind::object const& output = funct(table, object().lua_game_object(), object().lua_game_object() ? object().lua_game_object()->Parent() : nullptr);
			if (output && output.type() == LUA_TTABLE)
			{
				M = ::luabind::object_cast<LPCSTR>(output["anm_name"]);
				bMixIn = ::luabind::object_cast<bool>(output["anm_mixin"]);
				bMixIn2 = ::luabind::object_cast<bool>(output["anm_mixin2"]);
				state = ::luabind::object_cast<u32>(output["anm_state"]);
				speed = ::luabind::object_cast<float>(output["anm_speed"]);
				end = ::luabind::object_cast<float>(output["anm_end"]);
			}

			if (M == "$cancel")
			{
				m_sounds.StopAllSounds();
				if (GetState() != m_lastState)
					SwitchState(m_lastState);
				return 0;
			}
		}

		if (!HudAnimationExist(*M))
		{
			Msg("!Missing hud animation %s", *M);
			return 0;
		}
	}

	float final_speed = speed;
	PlayHUDMotion_noCB(M, bMixIn, speed, bMixIn2, &final_speed);

	float motion_length, def_speed, anim_end;
	bool stop_at_end;
	if (!GetStartedMotionInfo(M, motion_length, def_speed, stop_at_end, anim_end))
	{
		// Unknown motion: keep the old fallback of a 100 ms one-shot animation
		motion_length = .1f;
		def_speed = 1.f;
		stop_at_end = true;
		anim_end = 0.f;
	}

	++m_motion_serial;
	m_startedMotionState = state;
	m_bStopAtEndAnimIsRunning = stop_at_end;
	m_dwMotionLastUpdateTm = Device.dwTimeGlobal;
	m_fPreviousMotionTime = 0.f;
	m_fCurrentMotionTime = 0.f;
	m_fCurrentMotionLength = motion_length;
	m_fCurrentMotionSpeed = def_speed * final_speed; // the rate the skeleton actually plays it at

	// A stop-at-end animation is finished where the skeleton freezes it, unless an end trim is given.
	// End trims are real seconds cut from the end: 4th value of the anm_ line, the "end" argument of
	// play_hud_motion (anm_end in actor_on_hud_animation_play), or the g_end_modif console variable.
	float end_modifier = anim_end;
	if (end_modifier == 0.f)
		end_modifier = end;
	if (g_end_modif != 0.f)
		end_modifier = g_end_modif;

	m_fCurrentMotionEnd = motion_length - HUD_MOTION_END_EPS;
	if (stop_at_end && end_modifier != 0.f)
		m_fCurrentMotionEnd = motion_length - end_modifier * m_fCurrentMotionSpeed;

	// Return value as scripts know it: real duration in ms of a stop-at-end animation, 0 for a looping one
	if (!stop_at_end || m_fCurrentMotionSpeed <= 0.f)
		return 0;

	return iFloor(0.5f + 1000.f * motion_length / m_fCurrentMotionSpeed);
}

// Looks up the hand motion PlayHUDMotion_noCB just started (same random variant and 16:9 alias as on screen).
bool CHudItem::GetStartedMotionInfo(const shared_str& M, float& length, float& def_speed, bool& stop_at_end, float& anim_end)
{
	anim_end = 0.f;

	const player_hud_motion* anm = nullptr;
	if (IsAttachedToHUD())
	{
		anm = HudItemData()->find_motion(M);
		if (anm)
			anim_end = anm->m_anim_end; // the hud section end trim only ever applied on the player's HUD
	}
	else
	{
		player_hud_motion_container* pc = g_player_hud->get_hand_motions(*HudSection());
		if (pc)
			anm = pc->find_motion(M);
	}

	if (!anm || anm->m_animations.empty())
		return false;

	const u8 idx = m_started_rnd_anim_idx < anm->m_animations.size() ? m_started_rnd_anim_idx : 0;
	return g_player_hud->motion_info(anm->m_animations[idx].mid, m_current_motion_def, length, def_speed, stop_at_end);
}

bool CHudItem::PlayHUDMotion_noCB(const shared_str& motion_name, BOOL bMixIn, float speed, bool bMixIn2, float* final_speed)
{
	m_current_motion = motion_name;

	if (bDebug && item().m_pInventory)
	{
		Msg("-[%s] as[%d] [%d]anim_play [%s][%d]",
			IsAttachedToHUD() ? "HUD" : "Simulating",
		    item().m_pInventory->GetActiveSlot(),
		    item().object_id(),
		    motion_name.c_str(),
		    Device.dwFrame);
	}
	if (IsAttachedToHUD())
	{
		// Applies the hud section speed if the caller used the default one and writes the final speed back
		HudItemData()->anim_play(motion_name, bMixIn, m_current_motion_def, m_started_rnd_anim_idx, speed, bMixIn2);
	}
	else
	{
		m_started_rnd_anim_idx = 0;
	}

	if (final_speed)
		*final_speed = speed;

	return g_player_hud->motion_is_stop_at_end(motion_name, HudSection());
}

bool CHudItem::CurrentMotionHasMark(LPCSTR name) const
{
	if (!m_current_motion_def)
		return false;

	for (const motion_marks& M : m_current_motion_def->marks)
		if (!M.is_empty() && xr_strcmp(M.name.c_str(), name) == 0)
			return true;

	return false;
}

void CHudItem::StopCurrentAnimWithoutCallback()
{
	m_fPreviousMotionTime = 0.f;
	m_fCurrentMotionTime = 0.f;
	m_fCurrentMotionLength = 0.f;
	m_fCurrentMotionSpeed = 0.f;
	m_fCurrentMotionEnd = 0.f;
	m_bStopAtEndAnimIsRunning = false;
	m_current_motion_def = nullptr;
	++m_motion_serial;
}

BOOL CHudItem::GetHUDmode()
{
	if (object().H_Parent())
	{
		CActor* A = smart_cast<CActor*>(object().H_Parent());
		return (A && A->HUDview() && IsAttachedToHUD());
	}
	else
		return FALSE;
}

void CHudItem::PlayBlendAnm(LPCSTR name, float speed, float power, bool stop_old)
{
	u8 part = (object().cast_weapon()->IsZoomed() ? 2 : (g_player_hud->attached_item(1) ? 0 : 2));

	if (stop_old) g_player_hud->StopBlendAnm(name, true);
	g_player_hud->PlayBlendAnm(name, part, speed, power, false);
}

void CHudItem::PlayAnimIdle()
{
	if (TryPlayAnimIdle()) return;

	PlayHUDMotion("anm_idle", TRUE, NULL, GetState());
}

bool CHudItem::TryPlayAnimIdle()
{
	if (MovingAnimAllowedNow())
	{
		CActor* pActor = smart_cast<CActor*>(object().H_Parent());
		if (pActor && pActor->AnyMove())
		{
			if (pActor->is_safemode() && !smart_cast<CCustomDevice*>(this)) return false;

			CEntity::SEntityState st;
			pActor->g_State(st);
			if (st.bSprint)
			{
				PlayAnimIdleSprint();
				return true;
			}
			else if (!st.bCrouch)
			{
				PlayAnimIdleMoving();
				return true;
			}
#ifdef NEW_ANIMS //AVO: new crouch idle animation
			else if (st.bCrouch)
			{
				if (!PlayAnimCrouchIdleMoving())
					PlayHUDMotion("anm_idle_moving", TRUE, NULL, GetState(), .7f);
				return true;
			}
#endif //-NEW_ANIMS
		}
	}

	return false;
}

//AVO: check if animation exists
bool CHudItem::HudAnimationExist(LPCSTR anim_name)
{
	if (IsAttachedToHUD()) // First person
	{
		string256 anim_name_r;
		bool is_16x9 = UI().is_widescreen();
		u16 attach_place_idx = pSettings->r_u16(HudItemData()->m_sect_name, "attach_place_idx");
		xr_sprintf(anim_name_r, "%s%s", anim_name, ((attach_place_idx == 1) && is_16x9) ? "_16x9" : "");
		player_hud_motion* anm = HudItemData()->m_hand_motions->find_motion(anim_name_r);
		if (anm)
			return true;

		anm = HudItemData()->m_hand_motions->find_motion(anim_name);
		if (anm)
			return true;
	}
	else // Third person
	{
		if (g_player_hud->motion_length(anim_name, HudSection(), m_current_motion_def) > 100)
			return true;
	}
#ifdef DEBUG
    Msg("~ [WARNING] ------ Animation [%s] does not exist in [%s]", anim_name, HudSection().c_str());
#endif
	return false;
}

//-AVO

//AVO: new crouch idle animation
bool CHudItem::PlayAnimCrouchIdleMoving()
{
	if (HudAnimationExist("anm_idle_moving_crouch"))
	{
		PlayHUDMotion("anm_idle_moving_crouch", TRUE, NULL, GetState());
		return true;
	}
	return false;
}

//-AVO

bool CHudItem::NeedBlendAnm() 
{
	u32 state = GetState();
	return (state != eIdle && state != eHidden);
}

void CHudItem::PlayAnimIdleMoving()
{
	PlayHUDMotion("anm_idle_moving", TRUE, NULL, GetState(), isActorAccelerated(Actor()->MovingState(), false) ? 1.f : .75f);
}

#include "weapon.h"
#include "../xrEngine/SkeletonMotions.h"

void CHudItem::PlayAnimIdleSprint()
{
	PlayHUDMotion("anm_idle_sprint", TRUE, NULL, GetState());
}

void CHudItem::OnMovementChanged(ACTOR_DEFS::EMoveCommand cmd)
{
	if (GetState() == eIdle && !m_bStopAtEndAnimIsRunning)
	{
		if ((cmd == ACTOR_DEFS::mcSprint) || (cmd == ACTOR_DEFS::mcAnyMove) || (cmd == ACTOR_DEFS::mcCrouch) || (cmd == ACTOR_DEFS::mcAccel))
		{
			PlayAnimIdle();
			ResetSubStateTime();
		}
	}
}

extern shared_str current_player_hud_sect;

attachable_hud_item* CHudItem::HudItemData()
{
	if (!g_player_hud)
		return nullptr;

	if (!m_attachable)
	{
		current_player_hud_sect = hud_sect;
		m_attachable = xr_new<attachable_hud_item>(g_player_hud);
		m_attachable->m_parent_hud_item = this;
		m_attachable->load(hud_sect);
	}

	return m_attachable;
}

bool CHudItem::IsAttachedToHUD()
{
	if (!g_player_hud)
		return false;

	attachable_hud_item* hi = nullptr;

	hi = g_player_hud->attached_item(0);
	if (hi && hi->m_parent_hud_item == this)
		return true;

	hi = g_player_hud->attached_item(1);
	if (hi && hi->m_parent_hud_item == this)
		return true;

	hi = g_player_hud->attached_item(SCOPE_ATTACH_IDX);
	if (hi && hi->m_parent_hud_item == this)
		return true;

	return false;
}

bool CHudItem::ParentIsActor()
{
	CObject* O = object().H_Parent();
	if (!O)
		return false;

	CEntityAlive* EA = smart_cast<CEntityAlive*>(O);
	if (!EA)
		return false;

	return !!EA->cast_actor();
}

void CHudItem::ApplyAimModifiers(Fmatrix& matrix)
{
	// Fetch actor
	const CActor* pActor = Actor();

	// Fetch HUD pick
	const SPickParam& hud_pick = HUD().GetPick();

	// If firepos is disabled, use the eye position
	bool firepos = HUD().FireposActive();
	if (!firepos)
	{
		if (pActor->HUDview())
		{
			// If we're in first-person, use the HUD pick start position
			matrix.c = hud_pick.defs.start;
		}
		else
		{
			// If we're in third-person, project the actor position onto the HUD pick vector
			matrix.c = Fvector().mad(hud_pick.defs.start, hud_pick.defs.dir, Fvector().sub(pActor->Position(), hud_pick.defs.start).dotproduct(hud_pick.defs.dir));
		}
	}

	// If aim position is disabled...
	bool aimpos = HUD().AimposActive();
	if (!aimpos)
	{
		// Cache position
		Fvector pos = matrix.c;

		// Aim toward the hud pick's endpoint
		Fvector target = Fvector().mad(
			hud_pick.defs.start,
			hud_pick.defs.dir,
			hud_pick.defs.range
		);
		Fvector delta = Fvector().sub(target, pos).normalize();

		float h, p, b;
		delta.getHP(h, p);
		float _h, _p;
		Device.mInvView.getHPB(_h, _p, b);

		// Account for freelook offset
		if (pActor && pActor->cam_freelook != eflDisabled)
		{
			float cam_h, cam_p, cam_b;
			Device.mView.getHPB(cam_h, cam_p, cam_b);

			float pc = p;
			clamp(pc, 0.f, pc);

			h -= angle_normalize_signed(pActor->old_torso_yaw) - cam_h;
			p -= pc;
		}

		// Apply rotation
		matrix.setHPB(h, p, b);

		// Restore position
		matrix.c = pos;
	}
}

Fmatrix CHudItem::RayTransform()
{
	const attachable_hud_item* hi = HudItemData();
	Fmatrix matrix = hi->m_item_transform;
	matrix.mulB_43(hi->m_model->LL_GetTransform(0));

	ApplyAimModifiers(matrix);

	return matrix;
}

void CHudItem::Ray(SPickParam& pp)
{
	const CActor* pActor = Actor();
	if (!pActor)
		return;

	pp.InitPick();

	// Fetch transform, root bone matrix
	Fmatrix matrix = RayTransform();
	pp.barrel_matrix = matrix;

	// If we're in first-person...
	if (GetHUDmode())
	{
		// Build HUD projection without near-wall FOV offset for stability
		Fmatrix proj = Fmatrix().build_projection(
			deg2rad(GetBaseHudFov() * 83.f),
			Device.fASPECT,
			R_VIEWPORT_NEAR,
			g_pGamePersistent->Environment().CurrentEnv->far_plane
		);

		// Transform from non-offset HUD space to world space
		Device.hud_to_world(matrix, proj);
	}

	// Detect wall penetration
	Fvector eye_pos;

	// Start by choosing an eye position
	if (!GetHUDmode() && HUD().FireposActive())
	{
		// If we're in third-person with firepos active, use the actor's head bone
		eye_pos = pActor->XFORM().c;
		auto model = pActor->Visual()->dcast_PKinematics();
		eye_pos.add(model->LL_GetTransform(model->LL_BoneID("bip01_head")).c);
	}
	else
	{
		// Otherwise, use the camera
		eye_pos = Device.vCameraPosition;
	}

	// Trace from eye -> barrel
	SPickParam pn = SPickParam(CDB::OPT_CULL | CDB::OPT_ONLYFIRST);
	pn.defs.start = eye_pos;
	pn.defs.dir = Fvector3().sub(matrix.c, eye_pos);
	pn.defs.range = pn.defs.dir.magnitude();
	VERIFY(!fis_zero(pn.defs.range));
	pn.defs.dir.normalize();
	pp.barrel_dist = pn.defs.range;

	// If the eye -> barrel vector is obstructed...
	if (HUD().DoPick(pn))
	{
		pp.barrel_blocked = true;

		// If we're in first person...
		if (GetHUDmode())
		{
			// Use the eye -> barrel trace directly
			pp.defs.start = pn.defs.start;
			pp.defs.dir = pn.defs.dir;
			pp.defs.range = pn.defs.range;
		}
		else
		{
			// Move to the intersection point
			pn.defs.start.add(Fvector().mul(pn.defs.dir, pn.result.range * 0.99));

			// Trace to the camera
			pn.defs.dir = Fvector().sub(Device.vCameraPosition, pn.defs.start);
			pn.defs.range = pn.defs.dir.magnitude();
			pn.defs.dir.normalize();
			HUD().DoPick(pn);

			// Move to the intersection point and trace to the barrel
			pp.defs.start.add(pn.defs.start, Fvector().mul(pn.defs.dir, pn.result.range * 0.99));
			pp.defs.dir.sub(matrix.c, pp.defs.start);
			pp.defs.range = pp.defs.dir.magnitude();
			pp.defs.dir.normalize();
			pp.barrel_dist = pp.defs.range;
		}

		return;
	}

	// Trace from the resulting transform
	pp.defs.start = matrix.c;
	pp.defs.dir = matrix.k;
}

void CHudItem::UpdatePick()
{
	Ray(PP);
	HUD().DoPick(PP);
	if (PP.barrel_blocked)
		PP.result.range -= PP.barrel_dist;
}

void CHudItem::OnFrame()
{
	UpdatePick();

	if (g_nearwall)
		UpdateNearWall();

	float t = m_nearwall_speed_mod * Device.fTimeDelta;

	float target_fov = GetTargetHudFov();
	m_hud_fov = lerp(m_hud_fov, target_fov, t);

	float target_ofs = GetTargetNearWallOffset();
	m_nearwall_ofs = lerp(m_nearwall_ofs, target_ofs, t);
}

void CHudItem::net_Relcase(CObject* O)
{
	if (PP.result.O == O)
		PP.result.O = NULL;
}

float CHudItem::GetBaseHudFov()
{
	return (m_base_fov ? m_base_fov : psHUD_FOV_def) + m_hud_fov_add_mod;
}

float CHudItem::GetHudFov()
{
	return m_hud_fov;
}

float CHudItem::GetNearWallOffset()
{
	return m_nearwall_ofs;
}

CAnonHudItem::CAnonHudItem() { }

CAnonHudItem::~CAnonHudItem() { }

void CAnonHudItem::UpdateXForm() { }

void CAnonHudItem::on_renderable_Render(IDSGraphManager* DM) { }

void VisualCallbackHud(IKinematics* tpKinematics)
{
	CHudItem* game_object = static_cast<CHudItem*>((tpKinematics->GetUpdateCallbackParam()));
	VERIFY(game_object);

	CHudItem::HUD_CALLBACK_VECTOR_IT I = game_object->visual_callbacks().begin();
	CHudItem::HUD_CALLBACK_VECTOR_IT E = game_object->visual_callbacks().end();
	for (; I != E; ++I)
		(*I)(tpKinematics);
}

void CHudItem::add_visual_callback(hud_visual_callback* callback)
{
	if (!HudItemData()->m_model) return;
	HUD_CALLBACK_VECTOR_IT I = std::find(visual_callbacks().begin(), visual_callbacks().end(), callback);
	if (I != visual_callbacks().end()) return;
	if (m_visual_callback.empty()) SetKinematicsCallback(true);
	m_visual_callback.push_back(callback);
}

void CHudItem::remove_visual_callback(hud_visual_callback* callback)
{
	if (!HudItemData()->m_model) return;
	HUD_CALLBACK_VECTOR_IT I = std::find(m_visual_callback.begin(), m_visual_callback.end(), callback);
	if (I == m_visual_callback.end()) return;
	m_visual_callback.erase(I);
	if (m_visual_callback.empty()) SetKinematicsCallback(false);
}

void CHudItem::SetKinematicsCallback(bool set)
{
	if (!HudItemData()->m_model) return;
	if (set)
		HudItemData()->m_model->Callback(VisualCallbackHud, this);
	else
		HudItemData()->m_model->Callback(0, 0);
}
