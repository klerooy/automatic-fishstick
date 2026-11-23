#pragma once
#include <cstdint>

// GameAssembly.dll SHA-256: 03003754353ff08380dc8e91920f2c77f7977b370d413083acfe9b4bc65c18ab
// global-metadata.dat SHA-256: 23bed54126a2b89e9c78cfdbd59dc3be2bc14a5817823cf057cde33984618cc9
// Unity 6000.3.18f1, IL2CPP, metadata v39
// Source: github.com/klerooy/automatic-fishstick offsets/ dump 2026-11-12

namespace off {

// --- IL2CPP object layout (Unity 6 / IL2CPP x64) ---
constexpr uint32_t obj_header       = 0x10;
constexpr uint32_t class_name       = 0x10;
constexpr uint32_t class_ns         = 0x18;
constexpr uint32_t class_static     = 0xB8;
constexpr uint32_t str_len          = 0x10;
constexpr uint32_t str_chars        = 0x14;
constexpr uint32_t arr_len          = 0x18;
constexpr uint32_t arr_data         = 0x20;
constexpr uint32_t list_items       = 0x10;
constexpr uint32_t list_size        = 0x18;
constexpr uint32_t dict_buckets     = 0x10;
constexpr uint32_t dict_entries     = 0x18;
constexpr uint32_t dict_count       = 0x20;
constexpr uint32_t dict_entry_stride= 0x18;
constexpr uint32_t dict_value_off   = 0x10;

// --- Static field RVAs (fallback when il2cpp API unavailable) ---
constexpr uint32_t sf_world_model       = 0x81D36C8;
constexpr uint32_t sf_battle_camera     = 0x81CB420;
constexpr uint32_t sf_world_entities    = 0x81D0710;
constexpr uint32_t sf_battle_character  = 0x8196748;

// --- World chain (instance field offsets) ---
constexpr uint32_t world_entities       = 0x38; // WorldModel -> WorldEntitiesModel
constexpr uint32_t wem_battle_chars     = 0x18; // Dictionary<int, BattleCharacterModel>
constexpr uint32_t wem_vehicles         = 0x58;
constexpr uint32_t wem_loots            = 0x38;
constexpr uint32_t wem_containers       = 0x20;
constexpr uint32_t wem_doors            = 0x48;
constexpr uint32_t wem_cars             = 0x60;
constexpr uint32_t wem_helicopters      = 0x68;

// --- Entry chain (SDK) ---
constexpr uint32_t entry_game           = 0x28; // UnityEntry._game
constexpr uint32_t game_model           = 0xD0; // Game.GameModel
constexpr uint32_t game_battle          = 0x10; // GameModel._battleModel
constexpr uint32_t battle_entities      = 0x90; // BattleModel.BattleEntitiesModel
constexpr uint32_t battle_camera        = 0x358;// BattleModel.BattleCameraModel
constexpr uint32_t battle_world         = 0x60; // BattleEntitiesModel._worldEntitiesModel
constexpr uint32_t battle_player        = 0xD0; // BattleEntitiesModel.Player

// --- BattleCharacterModel ---
constexpr uint32_t char_id              = 0x38;
constexpr uint32_t char_data            = 0x40;
constexpr uint32_t char_config          = 0x48;
constexpr uint32_t char_transform       = 0x58;
constexpr uint32_t char_repl_transform  = 0x60;
constexpr uint32_t char_isdead          = 0xF0;
constexpr uint32_t char_private         = 0x10;
constexpr uint32_t char_weapon          = 0x150;
constexpr uint32_t char_crouch          = 0x160;
constexpr uint32_t char_stance          = 0x1F0;
constexpr uint32_t char_pbm             = 0x258;
constexpr uint32_t char_skeleton        = 0x138;
constexpr uint32_t char_inveh           = 0x1C8;
constexpr uint32_t char_state           = 0x1E0;
constexpr uint32_t char_look            = 0x70;

// --- CharacterTransformModel ---
constexpr uint32_t ctm_x                = 0x10;
constexpr uint32_t ctm_y                = 0x14;
constexpr uint32_t ctm_z                = 0x18;
constexpr uint32_t ctm_pitch            = 0x1C;
constexpr uint32_t ctm_yaw              = 0x20;
constexpr uint32_t ctm_roll             = 0x24;

// --- TransformModel (double-precision in Unity 6) ---
constexpr uint32_t transform_x          = 0x10;
constexpr uint32_t transform_y          = 0x18;
constexpr uint32_t transform_z          = 0x20;

// --- ReplicatedTransform ---
constexpr uint32_t rtransform_pos       = 0x10;
constexpr uint32_t rtransform_rx        = 0x18;
constexpr uint32_t rtransform_ry        = 0x20;
constexpr uint32_t rtransform_rz        = 0x28;

// --- Data chain ---
constexpr uint32_t data_common          = 0x20;
constexpr uint32_t data_private         = 0x28;
constexpr uint32_t data_public          = 0x30;
constexpr uint32_t common_role          = 0x20;
constexpr uint32_t private_health       = 0x38;
constexpr uint32_t public_armor         = 0x38;

// --- Health ---
constexpr uint32_t health_value         = 0x10;
constexpr uint32_t health_dead          = 0x18;
constexpr uint32_t health_max           = 0x20;

// --- Camera ---
constexpr uint32_t cam_pos              = 0x10;
constexpr uint32_t cam_rot              = 0x1C;
constexpr uint32_t cam_fwd              = 0x3C;
constexpr uint32_t cam_right            = 0x48;
constexpr uint32_t cam_up               = 0x54;
constexpr uint32_t cam_dist             = 0x60;

// --- Weapon ---
constexpr uint32_t weapon_item          = 0x10;
constexpr uint32_t crouch_bool          = 0x10;
constexpr uint32_t stance_ref           = 0x18;

// --- Generic wrappers ---
constexpr uint32_t prim_value           = 0x18;
constexpr uint32_t ref_value            = 0x18;
constexpr uint32_t desc_name            = 0x10;

// --- Position model ---
constexpr uint32_t pos_x                = 0x10;
constexpr uint32_t pos_y                = 0x14;
constexpr uint32_t pos_z                = 0x18;

// --- InVehicle ---
constexpr uint32_t inveh_id             = 0x10;
constexpr uint32_t inveh_type           = 0x20;

// --- Slot / nickname ---
constexpr uint32_t slot_nickname        = 0x30;
constexpr uint32_t slot_public_id       = 0x28;

// --- Spread ---
constexpr uint32_t priv_weapon_spread   = 0x228;
constexpr uint32_t client_fake_spread   = 0x2C0;
constexpr uint32_t fake_spread_values   = 0x10;

} // namespace off