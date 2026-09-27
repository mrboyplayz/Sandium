#include "LuaManager.hpp"

#include <memory>
#include <sol/sol.hpp>
#include <sstream>
#include <string>
#include <stdexcept>
#include <subhook.h>
#include <utility>
#include <thread>
#include <chrono>
#include <Windows.h>

#include "Addon.hpp"
#include "Addresses.hpp"
#include "LocalHuman.hpp"
#include "api/Logging.hpp"
#include "api/Http.hpp"
#include "api/Image.hpp"
#include "api/Time.hpp"
#include "api/Video.hpp"
#include "api/Image.hpp"
#include "api/Text.hpp"
#include "api/Billboards.hpp"
#include "api/Lighting.hpp"
#include "api/BloodMarks.hpp"
#include "ServerMedia.hpp"
#include "Roster.hpp"
#include "Flags.hpp"
#include "hooks/CreateItem.hpp"
#include "hooks/CreateVehicle.hpp"
#include "structs/Item.hpp"
#include "structs/ItemType.hpp"
#include "structs/Vehicle.hpp"
#include "structs/VehicleType.hpp"
#include "TypeManager.hpp"
#include "ItemModels.hpp"

// https://github.com/moonjit/moonjit/blob/master/doc/c_api.md#luajit_setmodel-idx-luajit_mode_wrapcfuncflag
static int wrapExceptions(lua_State* L, lua_CFunction f) 
{
	try
    {
		return f(L);
	} catch (const char* s)
    {
		lua_pushstring(L, s);
	} catch (const std::exception& e)
    {
		lua_pushstring(L, e.what());
	} catch (...)
    {
		lua_pushliteral(L, "caught (...)");
	}
	return lua_error(L);
}

LuaManager::LuaManager()
{

}

void LuaManager::Initialize()
{
    this->_L = std::make_unique<sol::state>();

    lua_pushlightuserdata(*this->_L, (void*)wrapExceptions);
	luaJIT_setmode(*this->_L, -1, LUAJIT_MODE_WRAPCFUNC | LUAJIT_MODE_ON);
	lua_pop(*this->_L, 1);

    this->_L->open_libraries(sol::lib::base);
	//this->_L->open_libraries(sol::lib::package);
	//this->_L->open_libraries(sol::lib::coroutine);
	this->_L->open_libraries(sol::lib::string);
	this->_L->open_libraries(sol::lib::math);
	this->_L->open_libraries(sol::lib::table);
	this->_L->open_libraries(sol::lib::bit32);

	(*this->_L)["print"] = [&](const sol::this_state &state, const sol::variadic_args &args)
	{
		std::stringstream stream;
		for (std::string str : args)
			stream << str << '\t';

		std::string text = stream.str();
		text.pop_back(); // Pop last tab
		this->GetCurrentAddon()->GetLogger()->LogText(text);
	};
	(*this->_L)["warn"] = [&](const sol::this_state &state, const sol::variadic_args &args)
	{
		std::stringstream stream;
		for (std::string str : args)
			stream << str << '\t';

		std::string text = stream.str();
		text.pop_back(); // Pop last tab
		this->GetCurrentAddon()->GetLogger()->LogText(std::string("<yellow>") + text);
	};

	this->_L->new_usertype<LuaHook>(
		"Hook",

		sol::call_constructor, [&](const std::string &hookName, const sol::function &hookFunction, const sol::variadic_args &flags)
		{
			std::shared_ptr<LuaHook> hook = std::make_shared<LuaHook>();

			hook->active = true;
			hook->addon = this->GetCurrentAddon();
			hook->name = hookName;
			hook->function = (sol::protected_function)hookFunction;
			if (flags.size() > 0)
			{
				for (std::string flag : flags)
					hook->flags.push_back(flag);
			}
			else
				hook->flags = std::vector<std::string>{ "pre" };

			this->_hooks.push_back(hook);

			return hook;
		},

		"Remove", &LuaHook::Remove
	);

	this->_L->new_usertype<Addon>(
		"Addon",
		sol::no_constructor,

		"isLoaded", sol::property(&Addon::IsLoaded),

		"id", sol::property(&Addon::ID),

		"name", sol::property(&Addon::GetName),
		"description", sol::property(&Addon::GetDescription),
		"logDecoration", sol::property(&Addon::GetLogDecoration),

		"GetAll", [&](const sol::this_state &state)
		{
			sol::table table = sol::table::create(state.L);
			std::size_t sz = 1;
			for (const std::unique_ptr<Addon> &addon : GetAddons())
			{
				table[sz] = addon.get();
				sz++;
			}
			return table;
		},
		"Current", sol::property([&]()
		{
			return this->GetCurrentAddon();
		})
	);

	this->_L->new_usertype<api::Image>(
		"Image",
		sol::no_constructor,
		"Load", &api::Image::Load,
		"Draw", sol::overload(
			[](api::Image &image, float x, float y) { image.Draw(x, y, static_cast<float>(image.Width()), static_cast<float>(image.Height())); },
			[](api::Image &image, float x, float y, float width, float height) { image.Draw(x, y, width, height); },
			[](api::Image &image, float x, float y, float width, float height, float alpha) { image.Draw(x, y, width, height, 1, 1, 1, alpha); },
			[](api::Image &image, float x, float y, float width, float height, float red, float green, float blue, float alpha) { image.Draw(x, y, width, height, red, green, blue, alpha); }
		),
		"Delete", &api::Image::Delete,
		"width", sol::property(&api::Image::Width),
		"height", sol::property(&api::Image::Height),
		"sizeX", sol::property(&api::Image::Width),
		"sizeY", sol::property(&api::Image::Height),
		"layer", sol::property(&api::Image::Layer, &api::Image::SetLayer),
		"rotation", sol::property(&api::Image::Rotation, &api::Image::SetRotation),
		"isValid", sol::property(&api::Image::IsValid)
	);

	this->_L->new_usertype<api::Video>(
		"Video",
		sol::no_constructor,
		"Load", &api::Video::Load,
		"Play", &api::Video::Play,
		"Pause", &api::Video::Pause,
		"Stop", &api::Video::Stop,
		"Draw", sol::overload(
			[](api::Video &video, float x, float y) { video.Draw(x, y, static_cast<float>(video.Width()), static_cast<float>(video.Height())); },
			[](api::Video &video, float x, float y, float width, float height) { video.Draw(x, y, width, height); },
			[](api::Video &video, float x, float y, float width, float height, float alpha) { video.Draw(x, y, width, height, 1, 1, 1, alpha); },
			[](api::Video &video, float x, float y, float width, float height, float red, float green, float blue, float alpha) { video.Draw(x, y, width, height, red, green, blue, alpha); }
		),
		"Delete", &api::Video::Delete,
		"width", sol::property(&api::Video::Width),
		"height", sol::property(&api::Video::Height),
		"sizeX", sol::property(&api::Video::Width),
		"sizeY", sol::property(&api::Video::Height),
		"layer", sol::property(&api::Video::Layer, &api::Video::SetLayer),
		"rotation", sol::property(&api::Video::Rotation, &api::Video::SetRotation),
		"isPlaying", sol::property(&api::Video::IsPlaying),
		"duration", sol::property(&api::Video::Duration),
		"position", sol::property(&api::Video::Position, &api::Video::SetPosition),
		"isValid", sol::property(&api::Video::IsValid)
	);

	this->_L->new_usertype<api::Time>(
		"Time",
		sol::no_constructor,
		"getSunAngle", &api::Time::GetSunAngle,
		"setSunAngle", &api::Time::SetSunAngle,
		"isValid", &api::Time::IsValid,
		"dumpDebug", &api::Time::DumpDebug,
		"getSunTime", &api::Time::GetSunTime,
		"setSunTime", &api::Time::SetSunTime
	);
	(*this->_L)["Time"] = api::Time{};

	this->_L->new_usertype<structs::Bone>(
		"Bone",
		sol::no_constructor,
		"rigidBodyID", &structs::Bone::rigidBodyID,
		"position", &structs::Bone::position,
		"velocity", &structs::Bone::velocity
	);

	this->_L->new_usertype<structs::Human>(
		"Human",
		sol::no_constructor,
		"isActive", &structs::Human::isActive,
		"playerID", &structs::Human::playerID,
		"position", &structs::Human::position,
		"viewYaw", &structs::Human::viewYaw,
		"viewPitch", &structs::Human::viewPitch,
		"inputFlags", &structs::Human::inputFlags,
		"health", &structs::Human::health,
		"damage", &structs::Human::damage,
		"bloodLevel", &structs::Human::bloodLevel,
		"isBleeding", sol::property([](structs::Human &human) { return human.isBleeding.b1; }),
		"index", [](structs::Human &human) -> int
		{
			return static_cast<int>(&human - addresses::Humans.ptr);
		},
		"headHealth", &structs::Human::headHealth,
		"chestHealth", &structs::Human::torsoHealth,
		"leftArmHealth", &structs::Human::leftArmHealth,
		"rightArmHealth", &structs::Human::rightArmHealth,
		"leftLegHealth", &structs::Human::leftLegHealth,
		"rightLegHealth", &structs::Human::rightLegHealth,
		"getBone", [](structs::Human &human, int index) -> structs::Bone &
		{
			if (index < 0 || index > 15)
				index = 0;
			return human.bones[index];
		},
		// Item currently in an inventory slot (0 left/offhand, 1 right/weapon
		// hand, 2+ pockets). Returns nil for an empty slot.
		"getSlotItem", [](structs::Human &human, int slot) -> structs::Item *
		{
			if (slot < 0 || slot > 5)
				return nullptr;
			const auto &s = human.inventorySlots[slot];
			int id = s.firstPlaceItemID;
			if (id < 0 || !addresses::Items[id].isActive.b1)
				id = s.secondPlaceItemID;
			if (id < 0 || !addresses::Items[id].isActive.b1)
				return nullptr;
			return &addresses::Items[id];
		}
	);

	// Use Sub Rosa's native bullet-hit event writer in local Practice Mode.
	// The game itself turns hit type 3 into blood particles/decal effects.
	auto nativeBlood = this->_L->create_named_table("NativeBlood");
	nativeBlood["Emit"] = [](const structs::CVector3 &position,
	                         const structs::CVector3 &normal)
	{
		if (*addresses::IsDedicated || !*addresses::IsInGame ||
		    !addresses::CreateEventBulletHitFunc.ptr)
			return false;
		structs::CVector3 hitPosition(position);
		structs::CVector3 hitNormal(normal);
		addresses::CreateEventBulletHitFunc(0, 3, &hitPosition, &hitNormal);
		return true;
	};
	nativeBlood["Ground"] = [](const structs::CVector3 &position)
	{
		if (*addresses::IsDedicated || !*addresses::IsInGame ||
		    !addresses::LineIntersectLevelFunc.ptr ||
		    !addresses::LineIntersectResult.ptr)
			return false;
		structs::CVector3 start(position.x, position.y + 0.3f, position.z);
		structs::CVector3 end(position.x, position.y - 3.0f, position.z);
		const structs::LineIntersectResult saved = *addresses::LineIntersectResult.ptr;
		const bool hit = addresses::LineIntersectLevelFunc(&start, &end, 1) != 0;
		const structs::LineIntersectResult result = *addresses::LineIntersectResult.ptr;
		*addresses::LineIntersectResult.ptr = saved;
		if (!hit) return false;
		api::bloodmarks::Add(result.position.x, result.position.y,
		                     result.position.z, result.normal.y);
		return true;
	};

	// World billboards: images/videos anchored to a world position,
	// stretched to width x height (world units), faded by opacity, hidden
	// beyond distance (0 = unlimited). Must be created from Lua; drawn
	// automatically every frame while in-game.
	this->_L->new_usertype<api::Billboard>(
		"Billboard",
		sol::no_constructor,
		"x", &api::Billboard::x,
		"y", &api::Billboard::y,
		"z", &api::Billboard::z,
		"width", &api::Billboard::width,
		"height", &api::Billboard::height,
		"distance", &api::Billboard::maxDistance,
		"opacity", &api::Billboard::opacity,
		"red", &api::Billboard::red,
		"green", &api::Billboard::green,
		"blue", &api::Billboard::blue,
		"visible", &api::Billboard::visible,
		"SetPosition", [](api::Billboard &b, float x, float y, float z) { b.x = x; b.y = y; b.z = z; },
		"Destroy", [](api::Billboard &b) { b.Destroy(); }
	);
	sol::table billboardsTable = this->_L->create_named_table("Billboards");
	billboardsTable["Create"] = [](sol::table opts)
	{
		auto billboard = std::make_shared<api::Billboard>();
		sol::object image = opts["image"];
		sol::object video = opts["video"];
		if (image.is<std::shared_ptr<api::Image>>())
			billboard->image = image.as<std::shared_ptr<api::Image>>();
		else if (image.is<std::string>())
			billboard->image = api::Image::Load(image.as<std::string>());
		else if (video.is<std::shared_ptr<api::Video>>())
			billboard->video = video.as<std::shared_ptr<api::Video>>();
		else
			return billboard;
		billboard->x = opts.get_or("x", 0.0f);
		billboard->y = opts.get_or("y", 0.0f);
		billboard->z = opts.get_or("z", 0.0f);
		billboard->width = opts.get_or("width", 2.0f);
		billboard->height = opts.get_or("height", 1.5f);
		billboard->maxDistance = opts.get_or("distance", 0.0f);
		billboard->opacity = opts.get_or("opacity", 1.0f);
		billboard->red = opts.get_or("r", 1.0f);
		billboard->green = opts.get_or("g", 1.0f);
		billboard->blue = opts.get_or("b", 1.0f);
		api::billboards::Register(billboard);
		return billboard;
	};
	billboardsTable["Clear"] = []() { api::billboards::Clear(); };

	// Lighting: additive glow light sources (XYZ or following a human).
	// Lighting.Create{ x=,y=,z=, r=,g=,b=, radius=, intensity=,
	//   human=<index>, offset={x,y,z}, ground=<bool> } -> Light
	// The engine shader only knows a sun direction, so these are Sandium-
	// rendered glows: depth-tested against the world, additive blending.
	this->_L->new_usertype<api::Light>(
		"Light",
		sol::no_constructor,
		"x", &api::Light::x,
		"y", &api::Light::y,
		"z", &api::Light::z,
		"radius", &api::Light::radius,
		"intensity", &api::Light::intensity,
		"source", &api::Light::source,
		"SetPosition", [](api::Light &light, float x, float y, float z) { light.SetPosition(x, y, z); },
		"SetColor", [](api::Light &light, float r, float g, float b) { light.SetColor(r, g, b); },
		"SetRadius", [](api::Light &light, float radius) { light.SetRadius(radius); },
		"SetIntensity", [](api::Light &light, float intensity) { light.SetIntensity(intensity); },
		"SetFollow", [](api::Light &light, int human, float ox, float oy, float oz) { light.SetFollow(human, ox, oy, oz); },
		"SetGroundPool", [](api::Light &light, bool enable) { light.SetGroundPool(enable); },
		"SetSourceVisible", [](api::Light &light, bool enable) { light.SetSourceVisible(enable); },
		"SetDirection", [](api::Light &light, float dx, float dy, float dz) { light.SetDirection(dx, dy, dz); },
		"SetCone", [](api::Light &light, float angleDegrees) { light.SetCone(angleDegrees); },
		"Destroy", [](api::Light &light) { light.Destroy(); }
	);
	sol::table lightingTable = this->_L->create_named_table("Lighting");
	lightingTable["Create"] = [](sol::table opts)
	{
		auto light = std::make_shared<api::Light>();
		light->x = opts.get_or("x", 0.0f);
		light->y = opts.get_or("y", 1.0f);
		light->z = opts.get_or("z", 0.0f);
		light->red = opts.get_or("r", 1.0f);
		light->green = opts.get_or("g", 1.0f);
		light->blue = opts.get_or("b", 1.0f);
		light->radius = opts.get_or("radius", 2.0f);
		light->intensity = opts.get_or("intensity", 1.0f);
		light->source = opts.get_or("source", true);
		light->groundPool = opts.get_or("ground", false);
		sol::object angle = opts["angle"];
		if (angle.is<float>() || angle.is<int>())
			light->SetCone(angle.is<float>() ? angle.as<float>() : static_cast<float>(angle.as<int>()));
		sol::object dir = opts["dir"];
		if (dir.is<sol::table>())
		{
			sol::table dirTable = dir.as<sol::table>();
			light->SetDirection(dirTable.get_or("x", 0.0f),
			                    dirTable.get_or("y", 0.0f),
			                    dirTable.get_or("z", 1.0f));
		}
		sol::object human = opts["human"];
		if (human.is<int>())
		{
			sol::object offset = opts["offset"];
			float ox = 0.0f, oy = 1.2f, oz = 0.0f;
			if (offset.is<sol::table>())
			{
				sol::table offsetTable = offset.as<sol::table>();
				ox = offsetTable.get_or("x", 0.0f);
				oy = offsetTable.get_or("y", 1.2f);
				oz = offsetTable.get_or("z", 0.0f);
			}
			light->SetFollow(human.as<int>(), ox, oy, oz);
		}
		api::lighting::Register(light);
		return light;
	};
	lightingTable["Clear"] = []() { api::lighting::Clear(); };
	// Master brightness multiplier for ALL lights (default 1, clamped 0.1-10).
	// Set per-frame or once; combines with each light's own intensity.
	lightingTable["SetBrightness"] = [](float brightness) { api::lighting::SetBrightness(brightness); };
	lightingTable["GetBrightness"] = []() { return api::lighting::GetBrightness(); };
	sol::table flagsTable = this->_L->create_named_table("ServerFlags");
	flagsTable["Get"] = [](const std::string &name) { return flags::Get(name); };

	// Custom UI API -- usable inside DrawHUD/DrawMenu hooks. Coordinates are
	// the game's 1024x768 UI space. Alignment: 0 right-anchored, 1 center,
	// 2 left, 3 up, 4 down (the game's own alignment codes).
	sol::table uiTable = this->_L->create_named_table("UI");
	uiTable["text"] = [](const std::string &text, float x, float y, float size,
	                     float r, float g, float b, float a, int alignment)
	{
		api::QueueText(text, x, y, size, glm::vec4(r, g, b, a),
		               static_cast<api::TextAlignment>(alignment));
	};
	uiTable["textShadow"] = [](const std::string &text, float x, float y, float size,
	                           float r, float g, float b, float a, int alignment)
	{
		api::QueueText(text, x, y, size, glm::vec4(r, g, b, a),
		               static_cast<api::TextAlignment>(alignment), true);
	};
	// Filled rectangle via the image layer (1x1 white texture, stretched).
	// The texture streams from the server (white.png in the manifest) -- the
	// addon sandbox cannot see core assets. Retries until it arrives; a
	// missing texture must never throw into the calling hook.
	uiTable["rect"] = [](float x, float y, float w, float h,
	                     float r, float g, float b, float a)
	{
		static std::shared_ptr<api::Image> white;
		if (!white || !white->IsValid())
		{
			const std::string path = servermedia::Path("white.png");
			if (path.empty())
				return;
			try
			{
				white = api::Image::Load(path);
			}
			catch (...)
			{
				return;
			}
			white->SetLayer(0); // foreground pass -- negative layers draw behind the world
		}
		white->Draw(x, y, w, h, r, g, b, a);
	};
	uiTable["mouse"] = []() {
		using MouseState = unsigned int (__cdecl *)(int *, int *);
		using WindowSize = void (__cdecl *)(void *, int *, int *);
		HMODULE sdl = GetModuleHandleA("SDL2.dll");
		auto mouse = sdl ? reinterpret_cast<MouseState>(GetProcAddress(sdl, "SDL_GetMouseState")) : nullptr;
		auto size = sdl ? reinterpret_cast<WindowSize>(GetProcAddress(sdl, "SDL_GetWindowSize")) : nullptr;
		int x = 0, y = 0, w = 1024, h = 768;
		unsigned int buttons = mouse ? mouse(&x, &y) : 0;
		if (size && addresses::SDLWindowPtr.ptr && *addresses::SDLWindowPtr)
			size(*addresses::SDLWindowPtr, &w, &h);
		// Sub Rosa scales both UI axes by window width / 1024; on 16:9
		// screens the visible virtual height is about 576, not 768.
		return std::make_tuple(w > 0 ? x * 1024.0f / w : 0.0f,
		                       w > 0 ? y * 1024.0f / w : 0.0f, (buttons & 1) != 0);
	};
	uiTable["keyPressed"] = [](int key) {
		return key >= 0 && key <= 255 && (GetAsyncKeyState(key) & 1) != 0;
	};
	uiTable["unixTime"] = []() {
		return std::chrono::duration<double>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	};
	uiTable["cursor"] = [](float x, float y) {
		static std::shared_ptr<api::Image> pointer;
		if (!pointer || !pointer->IsValid())
		{
			try { pointer = api::Image::LoadCore("texture/mouse01.png"); }
			catch (...) { return; }
			pointer->SetLayer(0);
		}
		// The game's cursor is stored in the center quadrant of a 128x128 atlas.
		pointer->Draw(x - 64, y - 64, 128, 128);
	};
	sol::table hideSeekTable = this->_L->create_named_table("HideSeek");
	hideSeekTable["Action"] = [](const std::string &action) {
		if (action != "ready" && action != "solo-seeker" && action != "solo-hider") return;
		std::thread([action]() {
			try { http::PostJson("185.227.111.150", 80, "/hs/action",
			    std::string("{\"action\":\"") + action + "\"}", 3000); }
			catch (...) {}
		}).detach();
	};

	sol::table gameTable = this->_L->create_named_table("Game");
	gameTable["isInGame"] = []() { return addresses::IsInGame.ptr && addresses::IsInGame.ptr->b1; };
	gameTable["isNoxus"] = &netredirect::IsNoxusGame;

	sol::table mediaTable = this->_L->create_named_table("ServerMedia");
	mediaTable["Sync"] = &servermedia::Sync;
	mediaTable["Ready"] = &servermedia::Ready;
	mediaTable["Path"] = &servermedia::Path;
	// Tick starts the download only after a Noxus game connection is active.
	roster::Start(); // name -> phone roster for name highlighting
	flags::Start(); // live server flags for streamed content

	sol::table humansTable = this->_L->create_named_table("Humans");
	humansTable["GetAll"] = [](const sol::this_state &state)
	{
		sol::table table = sol::table::create(state.L);
		std::size_t count = 1;
		for (std::size_t humanCount = 0; humanCount < structs::Human::VanillaCount; humanCount++)
			if (addresses::Humans[humanCount].isActive.b1)
				table[count++] = &addresses::Humans[humanCount];
		return table;
	};
	humansTable["GetLocal"] = []() -> structs::Human *
	{
		return localhuman::Find().first;
	};
	
	this->DefineGameTypes();
}
void LuaManager::Deinitialize()
{
	this->_hooks.clear();

    this->_L.reset();
}
void LuaManager::DefineGameTypes()
{
	this->_L->new_usertype<structs::CInteger>(
		"Integer",
		sol::no_constructor,

		"value", &structs::CInteger::i,

		"__tostring", &structs::CInteger::operator std::string
	);

	this->_L->new_usertype<structs::CBoolean>(
		"Boolean",
		sol::no_constructor,
		
		"value", &structs::CBoolean::b1,

		"__tostring", &structs::CBoolean::operator std::string
	);

	this->_L->new_usertype<structs::CVector3>(
		"Vector3",

		"x", &structs::CVector3::x,
		"y", &structs::CVector3::y,
		"z", &structs::CVector3::z,

		sol::call_constructor, []()
		{
			return structs::CVector3();
		},
		sol::call_constructor, [](float x, float y, float z)
		{
			return structs::CVector3(x, y, z);
		},

		"Set", &structs::CVector3::operator =,

		"length", sol::property(&structs::CVector3::Length),
		"Dot", &structs::CVector3::Dot,
		"Cross", &structs::CVector3::Cross,

		"__tostring", &structs::CVector3::operator std::string,

		"Zero", sol::property([]()
		{
			return (structs::CVector3)structs::CVector3(0.0f, 0.0f, 0.0f);
		}),
		"One", sol::property([]()
		{
			return (structs::CVector3)structs::CVector3(1.0f, 1.0f, 1.0f);
		})
	);

	this->_L->new_usertype<structs::COrientation>(
		"Orientation",

		"right", &structs::COrientation::right,
		"up",    &structs::COrientation::up,
		"back",  &structs::COrientation::back,

		sol::call_constructor, []()
		{
			return structs::COrientation();
		},

		"Set", &structs::COrientation::operator =,

		"__tostring", &structs::COrientation::operator std::string,

		"Identity", sol::property([]()
		{
			return structs::COrientation();
		})
	);

	this->_L->new_usertype<structs::ItemType>(
		"ItemType",
		sol::no_constructor,

		"name", sol::property(&structs::ItemType::GetName, &structs::ItemType::SetName),

		"index", sol::property(&structs::ItemType::GetIndex),
		"typeID", sol::property(&structs::ItemType::GetTypeID),
		"SetModel", sol::overload(
			[](structs::ItemType &itemType, const std::string &cmoPath,
			   const std::string &texturePath)
			{
				return itemmodels::Set(itemType, cmoPath, texturePath);
			},
			[](structs::ItemType &itemType, const std::string &cmoPath,
			   const std::string &texturePath, float meshScale, float forwardPitchDegrees)
			{
				return itemmodels::Set(itemType, cmoPath, texturePath,
				                       meshScale, forwardPitchDegrees);
			}),
		"SetTexture", [](structs::ItemType &itemType, const std::string &texturePath)
		{
			return itemmodels::SetTexture(itemType, texturePath);
		},

		"GetAll", [](const sol::this_state &state)
		{
			sol::table table = sol::table::create(state.L);
			const std::size_t itemTypeLimit = *addresses::IsDedicated
				? structs::ItemType::VanillaCount
				: structs::ItemType::ExpandedCount;
			for (std::size_t itemTypeCount = 0; itemTypeCount < itemTypeLimit; itemTypeCount++)
				table[itemTypeCount + 1] = &addresses::ItemTypes[itemTypeCount];
			return table;
		},
		"GetByID", [](const std::string &typeID)
		{
			std::pair<std::string, std::string> decomposed = DecomposeTypeID(typeID);
			return &addresses::ItemTypes[GetItemTypeManager()->GetID(decomposed.first, decomposed.second)];
		},
		"Register", [](const std::string &typeID, const structs::ItemType &baseType)
		{
			if (*addresses::IsDedicated)
				throw std::runtime_error("Expanded item types are not available in the dedicated server build yet");
			const auto decomposed = DecomposeTypeID(typeID);
			auto *manager = GetItemTypeManager();
			if (manager->HasID(decomposed.first, decomposed.second))
				throw std::logic_error("Item type is already registered: " + typeID);
			const std::size_t index = manager->GetNextID();
			if (index >= structs::ItemType::ExpandedCount)
				throw std::runtime_error("Expanded item type table is full (maximum ID is 63)");

			auto &definition = addresses::ItemTypes[index];
			auto *typeIDStorage = definition.customData.typeIDPtr;
			definition = baseType;
			definition.customData.index = static_cast<int>(index);
			definition.customData.typeIDPtr = typeIDStorage;
			*typeIDStorage = typeID;
			manager->RegisterID(decomposed.first, decomposed.second, index);
			return &definition;
		}
	);

	this->_L->new_usertype<structs::Item>(
		"Item",
		sol::no_constructor,

		"isActive", &structs::Item::isActive,
		"type", sol::property(&structs::Item::GetType),
		"typeID", &structs::Item::typeID,
		"parentHumanID", &structs::Item::parentHumanID,

		"position", &structs::Item::position,
		"velocity", &structs::Item::velocity,
		"orientation", &structs::Item::orientation,

		"Create", sol::overload(
			[](const structs::ItemType &itemType, const structs::CVector3 &position, const structs::COrientation &orientation)
			{
				structs::CVector3 realPosition = position;
				structs::CVector3 realVelocity = structs::CVector3();
				structs::COrientation realOrientation = orientation;

				subhook::ScopedHookRemove scopedRemove(createItemHook);
				int itemID = addresses::CreateItemFunc(itemType.customData.index, &realPosition, &realVelocity, &realOrientation);
				if (itemID < 0)
					throw std::runtime_error("Could not create item");
				return &addresses::Items[itemID];
			},
			[](const structs::ItemType &itemType, const structs::CVector3 &position, const structs::COrientation &orientation, const structs::CVector3 &velocity)
			{
				structs::CVector3 realPosition = position;
				structs::CVector3 realVelocity = velocity;
				structs::COrientation realOrientation = orientation;

				subhook::ScopedHookRemove scopedRemove(createItemHook);
				int itemID = addresses::CreateItemFunc(itemType.customData.index, &realPosition, &realVelocity, &realOrientation);
				if (itemID < 0)
					throw std::runtime_error("Could not create item");
				return &addresses::Items[itemID];
			})
	);

	this->_L->new_usertype<structs::VehicleType>(
		"VehicleType",
		sol::no_constructor,

		"name", sol::property(&structs::VehicleType::GetName, &structs::VehicleType::SetName),
		"price", &structs::VehicleType::price,
		"mass", &structs::VehicleType::mass,

		"index", sol::property(&structs::VehicleType::GetIndex),
		"typeID", sol::property(&structs::VehicleType::GetTypeID),

		"GetAll", [](const sol::this_state &state)
		{
			sol::table table = sol::table::create(state.L);
			for (std::size_t vehicleTypeCount = 0; vehicleTypeCount < structs::VehicleType::VanillaCount; vehicleTypeCount++)
				table[vehicleTypeCount + 1] = &addresses::VehicleTypes[vehicleTypeCount];
			return table;
		},
		"GetByID", [](const std::string &typeID)
		{
			std::pair<std::string, std::string> decomposed = DecomposeTypeID(typeID);
			return &addresses::VehicleTypes[GetVehicleTypeManager()->GetID(decomposed.first, decomposed.second)];
		}
	);

	this->_L->new_usertype<structs::Vehicle>(
		"Vehicle",
		sol::no_constructor,

		"isActive", &structs::Vehicle::isActive,
		"type", sol::property(&structs::Vehicle::GetType),

		"position", &structs::Vehicle::position,
		"velocity", &structs::Vehicle::velocity,
		"orientation", &structs::Vehicle::orientation,

		"Create", sol::overload(
			[](const structs::VehicleType &itemType, int colorID, const structs::CVector3 &position, const structs::COrientation &orientation)
			{
				structs::CVector3 realPosition = position;
				structs::CVector3 realVelocity = structs::CVector3();
				structs::COrientation realOrientation = orientation;

				subhook::ScopedHookRemove scopedRemove(createVehicleHook);
				int vehicleID = addresses::CreateVehicleFunc(itemType.customData.index, &realPosition, &realVelocity, &realOrientation, colorID);
				if (vehicleID < 0)
					throw std::runtime_error("Could not create vehicle");
				return &addresses::Vehicles[vehicleID];
			},
			[](const structs::VehicleType &itemType, int colorID, const structs::CVector3 &position, const structs::COrientation &orientation, const structs::CVector3 &velocity)
			{
				structs::CVector3 realPosition = position;
				structs::CVector3 realVelocity = velocity;
				structs::COrientation realOrientation = orientation;

				subhook::ScopedHookRemove scopedRemove(createVehicleHook);
				int vehicleID = addresses::CreateVehicleFunc(itemType.customData.index, &realPosition, &realVelocity, &realOrientation, colorID);
				if (vehicleID < 0)
					throw std::runtime_error("Could not create vehicle");
				return &addresses::Vehicles[vehicleID];
			})
	);
}

Addon *LuaManager::GetCurrentAddon()
{
	lua_State *L = this->_L->lua_state();
	lua_rawgeti(L, LUA_REGISTRYINDEX, LUAMANAGER_LUAADDONINDEX);
	void *ptr = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return (Addon *)ptr;
}
void LuaManager::SetCurrentAddon(Addon *addon)
{
	lua_State *L = this->_L->lua_state();
	lua_pushlightuserdata(L, addon);
	lua_rawseti(L, LUA_REGISTRYINDEX, LUAMANAGER_LUAADDONINDEX);
}

const std::vector<std::shared_ptr<LuaHook>> &LuaManager::GetHooks() const
{
	return this->_hooks;
}

void LuaManager::RemoveAddonHooks(Addon *addon)
{
	for (const auto &hook : this->_hooks)
		if (hook->addon == addon && hook->active)
			hook->Remove();
}
void LuaManager::CheckHooks()
{
	for (auto it = this->_hooks.begin(); it != this->_hooks.end(); ++it)
	{
		if (!(*it)->active)
		{
			this->_hooks.erase(it);
			break;
		}
	}
}

sol::state *LuaManager::L() const
{
    return this->_L.get();
}

LuaManager *GetMainLuaManager()
{
    static LuaManager s;
    return &s;
}
