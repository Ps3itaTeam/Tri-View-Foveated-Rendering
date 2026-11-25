#include <cstring>
#include <cctype>
#include <functional>
#include <chrono>
#include <cmath>
#include <algorithm>

#include "reframework/API.hpp"
#include "sdk/SceneManager.hpp"

#include "../VR.hpp"
#include "../Camera.hpp"

#include "HookManager.hpp"
#include "CameraDuplicator.hpp"
#include "QuadView.hpp"
#include "mods/vr/ReleaseBuild.hpp" // [RELEASE SWITCH fix140] REVR_RELEASE

QUADVIEW_BUILD_STAMP(CameraDuplicator); // [BUILD STAMP fix107]

namespace vrmod {
constexpr auto PRIORITY_OFFSET = 0x48;

void CameraDuplicator::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    {
        std::scoped_lock _{ m_camera_mutex };
        std::vector<REComponent*> cameras_to_remove{};

        for (auto camera : m_new_cameras) {
            //*(int32_t*)((uint8_t*)camera + PRIORITY_OFFSET) = *(int32_t*)((uint8_t*)get_old_camera_counterpart(camera) + PRIORITY_OFFSET) + 1; // causes the game to NOT use the new camera as the main camera
            *(int32_t*)((uint8_t*)camera + PRIORITY_OFFSET) = -1;

            if (camera->referenceCount == 1) {
                cameras_to_remove.push_back(camera);
                REVR_LOG(spdlog::info("Removing camera {:x}", (uintptr_t)camera));
            } else {
                static const auto game_object_t = sdk::find_type_definition("via.GameObject");
                static const auto get_Valid = game_object_t != nullptr ? game_object_t->get_method("get_Valid") : nullptr;
                auto owner = utility::re_component::get_game_object(camera);

                // if the owning game object was destroyed then well... destroy the camera
                if (owner == nullptr || (get_Valid != nullptr && !get_Valid->call<bool>(sdk::get_thread_context(), owner))) {
                    cameras_to_remove.push_back(camera);
                    REVR_LOG(spdlog::info("Removing camera (destroyed gameobject) {:x}", (uintptr_t)camera));
                }
            }
        }

        for (auto camera : cameras_to_remove) {
            VR::get()->notify_camera_destroyed((RECamera*)camera);

            m_new_cameras.erase((RECamera*)camera);
            m_old_cameras.erase((RECamera*)camera);
            m_seen_cameras.erase((RECamera*)camera);
            m_old_to_new_cameras.erase((RECamera*)camera);
            m_new_to_old_camera.erase((RECamera*)camera);
            m_clone_render_output.erase((RECamera*)camera);
            m_clone_output_id.erase((RECamera*)camera);
            m_property_jobs.erase((RECamera*)camera);

            // Remove the camera from every clone list it is in (a destroyed clone leaves a gap: the list is
            // rebuilt at the next clone_camera, which re-creates the missing clones)
            for (auto& [old_camera, clones] : m_old_to_new_cameras) {
                std::erase(clones, (RECamera*)camera);
            }

            std::erase_if(m_new_to_old_camera, [camera](const auto& pair) {
                return pair.second == camera;
            });

            utility::re_managed_object::release(camera);
        }
    }

    if (hash == "LockScene"_fnv) {
        if (VR::get()->is_hmd_active()) {
            force_fxaa_off();
        }

        // [STEREO FOVEA 27.10.2026] multipass only: one clone, or two / three for the quad view (VR::wanted_camera_clones)
        const auto wanted = VR::get()->wanted_camera_clones();

        if (!VR::get()->is_hmd_active() || wanted == 0) {
            std::scoped_lock _{ m_camera_mutex };
            for (auto camera : m_new_cameras) {
                auto game_object = utility::re_component::get_game_object((REComponent*)camera);

                if (game_object != nullptr) {
                    game_object->shouldDraw = false;
                    game_object->shouldUpdate = false;
                }
            }

            // [RT LIFETIME 12.11.2026] no clone draws (main menu canvas, headset off): their ray tracing components go
            sync_clone_ray_tracing(0);
            return;
        }

        hook_get_primary_camera();
        clone_camera(wanted);
        apply_output_ids();
        sync_clone_ray_tracing(wanted);
        sync_clone_poses();

#if !REVR_RELEASE // [RELEASE SWITCH fix141] the camera check (a log): debug build only
        if (quadview::State::get().camera_check_request.exchange(false)) {
            log_camera_check();
        }
#endif
    }
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [CAMERA CHECK 10.11.2026] Effects of the game camera missing in the clones' views (RE2, dump of 10.11.2026: the contact
// shading under objects in the shared wide view only, without ray tracing; with the plain two-eye multipass in one eye
// only). Logs the components of the game camera and of each clone (enabled or not), and for every copied component each
// property whose value differs between the game camera and the clone (values of plain types; structures by their bytes;
// references only when one of them is null). Reads only.
void CameraDuplicator::log_camera_check() {
    // the render layers under each view's scene layer (type, enabled, ids), game camera's view first: a pass the engine
    // gives the game camera's view only shows as a layer the clones' views do not have
    {
        const auto scenes = get_relevant_scene_layers();
        std::function<void(sdk::renderer::RenderLayer*, int, std::string&)> walk = [&](sdk::renderer::RenderLayer* l, int depth, std::string& out) {
            if (l == nullptr || depth > 5) {
                return;
            }

            const auto t = utility::re_managed_object::get_type_definition(l);
            const auto enabled = t != nullptr ? t->get_method("get_Enabled") : nullptr;
            out += std::format("{}{}{} id {} out {}/{} prio {}", depth == 0 ? "" : " | ", std::string((size_t)depth, '>'), t != nullptr ? t->get_full_name() : std::string{"?"},
                l->m_id, l->m_render_output_id, l->m_render_output_id_2, l->m_priority);

            if (enabled != nullptr) {
                out += enabled->call<bool>(sdk::get_thread_context(), l) ? "" : " (disabled)";
            }

            auto& children = l->get_layers();

            for (auto i = 0; i < children.size() && i < 64; ++i) {
                walk(children[i], depth + 1, out);
            }
        };

        for (size_t i = 0; i < scenes.size(); ++i) {
            std::string tree{};
            walk(scenes[i], 0, tree);
            const auto cam = scenes[i] != nullptr ? scenes[i]->get_camera() : nullptr;
            auto go = cam != nullptr ? utility::re_component::get_game_object((REComponent*)cam) : nullptr;
            REVR_LOG(spdlog::info("[CameraDuplicator] camera check: scene layer {} (view {}, camera {}): {}", i, scenes[i] != nullptr ? scenes[i]->get_view_id() : 0,
                go != nullptr ? utility::re_string::get_string(go->name) : std::string{"?"}, tree));
        }
    }

    // the names of the RenderOutputID values (the game camera's view and the clones' use different ones)
    if (const auto e = sdk::find_type_definition("via.render.RenderOutputID"); e != nullptr) {
        std::string names{};

        for (auto f : e->get_fields()) {
            const auto flags = f->get_flags();

            if ((flags & (uint16_t)via::clr::FieldFlag::Static) != 0 && (flags & (uint16_t)via::clr::FieldFlag::Literal) != 0) {
                auto raw = f->get_data_raw(nullptr, true);
                int64_t v = 0;

                if (raw != nullptr) {
                    switch (e->get_valuetype_size()) {
                    case 1:
                        v = *(int8_t*)raw;
                        break;
                    case 2:
                        v = *(int16_t*)raw;
                        break;
                    case 8:
                        v = *(int64_t*)raw;
                        break;
                    default:
                        v = *(int32_t*)raw;
                        break;
                    }
                }

                names += std::format("{}={}, ", f->get_name(), v);
            }
        }

        REVR_LOG(spdlog::info("[CameraDuplicator] camera check: via.render.RenderOutputID values: {}", names));
    }

    std::scoped_lock _{ m_camera_mutex };
    auto ctx = sdk::get_thread_context();

    auto list_components = [](::REGameObject* go) {
        std::string s{};

        if (go == nullptr || go->transform == nullptr) {
            return std::string{"(none)"};
        }

        for (auto c = go->transform->childComponent; c != nullptr; c = c->childComponent) {
            const auto t = utility::re_managed_object::get_type_definition(c);
            const auto enabled = t != nullptr ? t->get_method("get_Enabled") : nullptr;
            s += t != nullptr ? t->get_full_name() : std::string{"?"};

            if (enabled != nullptr) {
                s += enabled->call<bool>(sdk::get_thread_context(), c) ? "" : " (disabled)";
            }

            s += ", ";
        }

        return s;
    };

    auto value_text = [](sdk::RETypeDefinition* rt, const reframework::InvokeRet& v) -> std::string {
        if (rt == nullptr) {
            return "void";
        }

        const auto n = rt->get_full_name();

        if (n == "System.Boolean") {
            return v.byte != 0 ? "true" : "false";
        }

        if (n == "System.Single") {
            return std::format("{:.6g}", v.f);
        }

        if (n == "System.Double") {
            return std::format("{:.6g}", v.d);
        }

        if (rt->is_enum() || rt->is_primitive()) {
            const auto size = rt->get_valuetype_size();
            return std::format("{}", size >= 8 ? (int64_t)v.qword : size >= 4 ? (int64_t)(int32_t)v.dword : size >= 2 ? (int64_t)(int16_t)v.word : (int64_t)v.byte);
        }

        // structures of up to 16 bytes as floats (vectors, colours), larger ones by their size
        const auto size = rt->get_valuetype_size();

        if (size > 0 && size <= 16 && size % 4 == 0) {
            std::string s{"("};

            for (size_t i = 0; i < (size_t)size / 4; ++i) {
                float f{};
                std::memcpy(&f, v.bytes.data() + i * 4, 4);
                s += std::format("{}{:.4g}", i == 0 ? "" : " ", f);
            }

            return s + ")";
        }

        return std::format("(struct {} bytes)", size);
    };

    for (auto& [old_camera, clones] : m_old_to_new_cameras) {
        auto main_go = old_camera != nullptr ? utility::re_component::get_game_object((REComponent*)old_camera) : nullptr;

        if (main_go == nullptr || main_go->transform == nullptr) {
            continue;
        }

        REVR_LOG(spdlog::info("[CameraDuplicator] camera check: game camera {} components: {}", utility::re_string::get_string(main_go->name), list_components(main_go)));
        REVR_LOG(spdlog::info("[CameraDuplicator] camera check: game camera priority {} type {}", *(int32_t*)((uint8_t*)old_camera + PRIORITY_OFFSET),
            *(int32_t*)((uint8_t*)old_camera + PRIORITY_OFFSET + 8)));

        for (size_t ci = 0; ci < clones.size(); ++ci) {
            if (clones[ci] != nullptr) {
                REVR_LOG(spdlog::info("[CameraDuplicator] camera check: clone {} priority {} type {}", ci, *(int32_t*)((uint8_t*)clones[ci] + PRIORITY_OFFSET),
                    *(int32_t*)((uint8_t*)clones[ci] + PRIORITY_OFFSET + 8)));
            }
        }

        for (size_t ci = 0; ci < clones.size(); ++ci) {
            auto clone_go = clones[ci] != nullptr ? utility::re_component::get_game_object((REComponent*)clones[ci]) : nullptr;

            if (clone_go == nullptr || clone_go->transform == nullptr) {
                continue;
            }

            REVR_LOG(spdlog::info("[CameraDuplicator] camera check: clone {} components: {} | draw {} update {}", ci, list_components(clone_go), clone_go->shouldDraw,
                clone_go->shouldUpdate));

            // every component the clone has (also the ones whose values are not copied: RenderOutput, CapturePlane ...)
            std::vector<std::string> names{};

            for (auto c = clone_go->transform->childComponent; c != nullptr; c = c->childComponent) {
                if (const auto ct = utility::re_managed_object::get_type_definition(c); ct != nullptr) {
                    names.push_back(ct->get_full_name());
                }
            }

            for (const auto& comp_name : names) {
                const struct {
                    std::string name;
                } descriptor{comp_name};
                auto a = utility::re_component::find<REComponent>(main_go->transform, descriptor.name);
                auto b = utility::re_component::find<REComponent>(clone_go->transform, descriptor.name);

                if (a == nullptr || b == nullptr) {
                    if (a != nullptr || b != nullptr) {
                        REVR_LOG(spdlog::info("[CameraDuplicator] camera check: clone {} {}: on the game camera {}, on the clone {}", ci, descriptor.name, a != nullptr,
                            b != nullptr));
                    }

                    continue;
                }

                const auto t = utility::re_managed_object::get_type_definition(a);

                if (t == nullptr) {
                    continue;
                }

                size_t count = 0;
                std::string diff{};

                for (auto& method : t->get_methods()) {
                    const std::string name{method.get_name()};

                    if (name.find("get_") != 0 || method.get_num_params() != 0) {
                        continue;
                    }

                    const auto rt = method.get_return_type();
                    std::array<void*, 0> none{};
                    const auto va = method.invoke(a, none);
                    const auto vb = method.invoke(b, none);
                    ++count;

                    if (va.exception_thrown || vb.exception_thrown) {
                        continue;
                    }

                    bool differs = false;
                    std::string what{};

                    if (rt != nullptr && rt->is_value_type()) {
                        const size_t size = (std::min<size_t>)(rt->get_valuetype_size(), sizeof(va.bytes));
                        differs = size > 0 && std::memcmp(va.bytes.data(), vb.bytes.data(), size) != 0;
                        what = std::format("{} / {}", value_text(rt, va), value_text(rt, vb));
                    } else if (rt != nullptr) {
                        differs = (va.ptr == nullptr) != (vb.ptr == nullptr);
                        what = std::format("{} / {}", va.ptr != nullptr ? "object" : "null", vb.ptr != nullptr ? "object" : "null");
                    }

                    if (differs) {
                        diff += std::format("{} ({}), ", name.substr(4), what);
                    }
                }

                REVR_LOG(spdlog::info("[CameraDuplicator] camera check: clone {} {}: {} properties read, differing (game camera / clone): {}", ci, descriptor.name, count,
                    diff.empty() ? std::string{"none"} : diff));
            }
        }
    }
}
#endif

// [CLONE POSE 09.11.2026] The clones are children of the game camera's transform with an identity local pose, so the
// engine's hierarchy update places them where the game camera is at that moment. A mod that places the game camera
// after that update, by writing its world matrix (and first joint) directly, moves only the game camera: RE2's first
// person mode does this in the player's transform update (FirstPerson::update_camera_transform). The clones then render
// from the game's own third-person camera while the game camera renders from the head (dump of 09.11.2026, RE2: the
// fovea views showed the player's head and shoulder from behind, the shared wide view the first-person view). Before
// the scene is locked for rendering the clones get the game camera's world pose as it is then.
void CameraDuplicator::sync_clone_poses() {
    std::scoped_lock _{ m_camera_mutex };

    for (auto& [old_camera, clones] : m_old_to_new_cameras) {
        auto src_go = old_camera != nullptr ? utility::re_component::get_game_object((REComponent*)old_camera) : nullptr;

        if (src_go == nullptr || src_go->transform == nullptr) {
            continue;
        }

        const auto& src = *src_go->transform;
        auto src_joint = utility::re_transform::get_joint(src, 0);

        for (auto clone : clones) {
            auto go = clone != nullptr ? utility::re_component::get_game_object((REComponent*)clone) : nullptr;

            if (go == nullptr || go->transform == nullptr) {
                continue;
            }

            go->transform->worldTransform = src.worldTransform;

            if (src_joint != nullptr) {
                if (auto joint = utility::re_transform::get_joint(*go->transform, 0); joint != nullptr) {
                    sdk::set_joint_rotation(joint, sdk::get_joint_rotation(src_joint));
                    sdk::set_joint_position(joint, sdk::get_joint_position(src_joint));
                }
            }
        }
    }
}

void CameraDuplicator::on_application_entry(void* entry, const char* name, size_t hash) {
    // Probably the best place to do it, it doesn't waste unnecessary time
    // after waiting for the render thread to finish, it does it before waiting
    if (hash == "LockScene"_fnv) {
        if (!VR::get()->is_hmd_active()) {
            return;
        }

        const auto wanted = VR::get()->wanted_camera_clones();

        if (wanted == 0) {
            return;
        }

        copy_camera_properties(wanted);
    }
}

void CameraDuplicator::on_draw_ui() {
#if !REVR_RELEASE // [RELEASE SWITCH fix140] developer tool: debug build only
    if (ImGui::TreeNode("Camera Duplicator")) {
        ImGui::Checkbox("Copy camera properties", &m_copy_camera);
        for (auto& descriptor : m_wanted_components) {
            ImGui::Checkbox(descriptor.name.c_str(), &descriptor.allowed);
        }
        ImGui::TreePop();
    }
#endif
}

size_t CameraDuplicator::get_active_clone_count() {
    const auto primary_camera = sdk::get_primary_camera();

    if (primary_camera == nullptr) {
        return 0;
    }

    return get_clone_count(primary_camera);
}

std::vector<sdk::renderer::layer::Scene*> CameraDuplicator::get_relevant_scene_layers() {
    const auto output_layer = sdk::renderer::get_output_layer();

    if (output_layer == nullptr) {
        return {};
    }

    auto scene_layers = output_layer->find_fully_rendered_scene_layers();

    if (scene_layers.size() < 2) {
        return scene_layers;
    }

    const auto primary_camera = sdk::get_primary_camera();

    if (primary_camera == nullptr) {
        return scene_layers;
    }

    std::vector<RECamera*> order{};
    order.push_back(primary_camera);

    {
        std::scoped_lock _{ m_camera_mutex };

        if (auto it = m_old_to_new_cameras.find(primary_camera); it != m_old_to_new_cameras.end()) {
            for (auto clone : it->second) {
                order.push_back(clone);
            }
        }
    }

    if (order.size() < 2) {
        return scene_layers;
    }

    // [primary, clone 0, clone 1, ...] first, every other layer after them in its own order
    size_t slot = 0;

    for (auto wanted_camera : order) {
        if (slot >= scene_layers.size()) {
            break;
        }

        for (size_t i = slot; i < scene_layers.size(); ++i) {
            if (scene_layers[i]->get_camera() == wanted_camera) {
                if (i != slot) {
                    std::swap(scene_layers[slot], scene_layers[i]);
                }

                ++slot;
                break;
            }
        }
    }

    return scene_layers;
}

void CameraDuplicator::clone_camera(size_t wanted) {
    // Untested on older than TDB 69 (RE8)
#if TDB_VER >= 69
    if (m_waiting_for_identify) {
        return;
    }

    wanted = (std::min)(wanted, MAX_CLONES);

    size_t num_main_cameras = 0;

    const auto main_camera = sdk::get_primary_camera();

    if (main_camera == nullptr) {
        return;
    }

    std::scoped_lock _{ m_camera_mutex };

    const auto seen_contains = m_seen_cameras.contains(main_camera);
    const auto existing_clones = get_clone_count(main_camera);

    if (seen_contains && existing_clones >= wanted) {
        return;
    }

    if (seen_contains && existing_clones > 0) {
        REVR_LOG(spdlog::info("Existing camera has {} clone(s), {} wanted: cloning again", existing_clones, wanted));
    } else if (seen_contains) {
        REVR_LOG(spdlog::info("Existing camera had its clone destroyed, re-cloning it"));
    }

    const auto camera_gameobject = utility::re_component::get_game_object((REComponent*)main_camera);

    if (camera_gameobject == nullptr) {
        return;
    }

    const auto current_main_camera_name = utility::re_string::get_view(camera_gameobject->name);

    const auto existing_cameras = get_all_cameras();

    if (existing_cameras == nullptr) {
        return;
    }

    REVR_LOG(spdlog::info("Found {} existing cameras", existing_cameras->size()));
    REVR_LOG(spdlog::info("Cloning {} @ 0x{:x} (clone {} of {})", utility::narrow(current_main_camera_name), (uintptr_t)main_camera, existing_clones + 1, wanted));

    for (::REManagedObject* camera : *existing_cameras) {
        if (camera == nullptr) {
            continue;
        }

        const auto gameobject = utility::re_component::get_game_object((REComponent*)camera);

        if (gameobject == nullptr) {
            continue;
        }

        if (!m_seen_cameras.contains((RECamera*)camera)) {
            utility::re_managed_object::add_ref(camera);
            m_seen_cameras.insert((RECamera*)camera);
            m_old_cameras.insert((RECamera*)camera);
        }

        const auto name = utility::re_string::get_view(gameobject->name);

        if (name == current_main_camera_name) {
            ++num_main_cameras;
        }
    }

    REVR_LOG(spdlog::info("Found {} main cameras", num_main_cameras));

    const auto old_camera_gameobject = utility::re_component::get_game_object((REComponent*)main_camera);

    if (old_camera_gameobject == nullptr) {
        return;
    }

    // Setting active to false lets us "activate" the folder again, cloning the camera perfectly
    // ADDENDUM: We don't do this anymore, and instead opt for manually recreating the components
    // as it causes a bunch of issues like the game thinks the new camera is the main one and other things.
    //constexpr auto ACTIVE_OFFSET = sizeof(::REManagedObject) + 5;
    //*(bool*)((uint8_t*)old_camera_folder + ACTIVE_OFFSET) = false;
    //sdk::call_object_func_easy<void*>(old_camera_folder, "activate");

    // Manually create the game object and duplicate the components
    static const auto gameobject_t = sdk::find_type_definition("via.GameObject");
    static const auto create_gameobject_fn = gameobject_t->get_method("create(System.String)");
    static const auto create_component_fn = gameobject_t->get_method("createComponent(System.Type)");

    // One clone per call: the next LockScene creates the next one (keeps each LockScene short)
    const size_t clone_index = existing_clones;

    // Create new gameobject. The first clone keeps its old name, the others are numbered.
    const auto adjusted_name = clone_index == 0 ? std::wstring{current_main_camera_name} + L" (Clone)"
                                                : std::wstring{current_main_camera_name} + L" (Clone " + std::to_wstring(clone_index + 1) + L")";
    const auto new_camera_gameobject = (::REGameObject*)create_gameobject_fn->call<sdk::ManagedObject*>(sdk::get_thread_context(), sdk::VM::create_managed_string(adjusted_name));
    new_camera_gameobject->shouldDraw = false;
    new_camera_gameobject->shouldUpdate = false;
    //utility::re_managed_object::add_ref(new_camera_gameobject);

    REVR_LOG(spdlog::info("Created new gameobject @ 0x{:x}", (uintptr_t)new_camera_gameobject));

    // Parent new gameobject to old gameobject
    const auto new_transform = new_camera_gameobject->transform;
    const auto old_transform = old_camera_gameobject->transform;

    if (new_transform != nullptr && old_transform != nullptr) {
        sdk::call_object_func_easy<void*>(new_transform, "setParent(via.Transform, System.Boolean)", old_transform, false);
        REVR_LOG(spdlog::info("Parented new camera to old camera"));
    }

    REComponent* component = old_camera_gameobject->transform->childComponent;

    std::unordered_set<std::string> illegal_components {
        "via.render.ExperimentalRayTrace",
        "via.motion.MotionCamera", // Unnecessarily takes control of the camera transform which we don't want
        "via.motion.ActorMotionCamera", // Unnecessarily takes control of the camera transform which we don't want
        "via.wwise.WwiseListener",
        "via.physics.Colliders",
        // todo
    };

    std::unordered_set<std::string> game_framework_components {
        "worldtour.bWTCameraController", // this causes the camera to become the "main" camera which is NOT what we want, it breaks the real main camera
        "camera.MainCameraController", // also breaks the real main camera
        "camera.CameraSystem", // also breaks the real main camera
        "bPostProcessController",
        "MainCameraDestroyChecker",
    };

    // Add new components to illegal_components with the game project name prepended to the game_framework_components
    for (auto& fwcomp : game_framework_components) {
        illegal_components.insert(game_namespace((fwcomp)));
    }

    const int32_t output_id = m_wanted_output_ids[(std::min)(clone_index, MAX_CLONES - 1)];
    RECamera* new_camera = nullptr;
    ::REManagedObject* render_output = nullptr;

    while (component != nullptr) {
        const auto tdef = utility::re_managed_object::get_type_definition(component);
        const auto runtime_type = tdef->get_runtime_type();

        if (illegal_components.contains(tdef->get_full_name())) {
            REVR_LOG(spdlog::info("Skipping illegal component {}", tdef->get_full_name()));
            component = component->childComponent;
            continue;
        }

        const auto full_name = tdef->get_full_name();

        // TODO: other game framework prefixes or maybe app is just okay?
        // snow is for MHRise, app is for most of the other games
        if (full_name.starts_with("app.") || full_name.starts_with("snow.")) {
            REVR_LOG(spdlog::info("Skipping app component {}", tdef->get_full_name()));
            component = component->childComponent;
            continue;
        }

        const auto new_comp = create_component_fn->call<sdk::ManagedObject*>(sdk::get_thread_context(), new_camera_gameobject, runtime_type);

        if (tdef->get_full_name() == "via.render.RenderOutput") {
            // [STEREO FOVEA] clone 0 has always had 3; the others get the ids of the test setting
            sdk::call_object_func_easy<void*>(new_comp, "set_RenderOutputID(via.render.RenderOutputID)", output_id);
            render_output = (::REManagedObject*)new_comp;
            REVR_LOG(spdlog::info("Clone {}: RenderOutputID {}", clone_index, output_id));
        }

        if (tdef->get_full_name() == "via.Camera") {
            new_camera = (RECamera*)new_comp;
            m_seen_cameras.insert(new_camera);
            m_new_cameras.insert(new_camera);
            m_old_to_new_cameras[(RECamera*)component].push_back(new_camera);
            m_new_to_old_camera[new_camera] = (RECamera*)component;
            //*(int32_t*)((uint8_t*)new_comp + PRIORITY_OFFSET) = *(int32_t*)((uint8_t*)get_old_camera_counterpart((RECamera*)new_comp) + PRIORITY_OFFSET) + 1; // causes the game to NOT use the new camera as the main camera
            *(int32_t*)((uint8_t*)new_comp + PRIORITY_OFFSET) = -1;

            utility::re_managed_object::add_ref(new_comp);
        }

        REVR_LOG(spdlog::info("Created new component {} @ 0x{:x}", tdef->get_full_name(), (uintptr_t)new_comp));

        component = component->childComponent;
    }

    if (new_camera != nullptr) {
        m_clone_render_output[new_camera] = render_output;
        m_clone_output_id[new_camera] = output_id;
        m_property_jobs.erase(new_camera);
    }

    new_camera_gameobject->shouldDraw = true; // YES draw by default, this is the counterpart to the main camera

    m_called_activate = true;
    m_identified_new_cameras = true;

    // Destroy our existing property jobs so we don't work with stale data
    m_getter_setters.clear();
    m_property_jobs.clear();

    /*m_identified_new_cameras = false;
    m_waiting_for_identify = true;

    find_new_cameras();*/
#endif
}

// [STEREO FOVEA] the RenderOutputID setting changed after the clones were made: write the new ids into the existing
// clones' RenderOutput components (whether the engine picks that up at runtime is logged by the test)
void CameraDuplicator::apply_output_ids() {
    std::scoped_lock _{ m_camera_mutex };

    const auto main_camera = sdk::get_primary_camera();

    if (main_camera == nullptr) {
        return;
    }

    auto it = m_old_to_new_cameras.find(main_camera);

    if (it == m_old_to_new_cameras.end()) {
        return;
    }

    for (size_t i = 0; i < it->second.size() && i < MAX_CLONES; ++i) {
        auto clone = it->second[i];
        const auto wanted = m_wanted_output_ids[i];
        auto id_it = m_clone_output_id.find(clone);
        auto ro_it = m_clone_render_output.find(clone);

        if (id_it == m_clone_output_id.end() || ro_it == m_clone_render_output.end() || ro_it->second == nullptr || id_it->second == wanted) {
            continue;
        }

        sdk::call_object_func_easy<void*>(ro_it->second, "set_RenderOutputID(via.render.RenderOutputID)", wanted);
        REVR_LOG(spdlog::info("Clone {}: RenderOutputID changed {} -> {}", i, id_it->second, wanted));
        id_it->second = wanted;
    }
}

void CameraDuplicator::hook_get_primary_camera() {
    if (m_hooked_get_primary_camera) {
        return;
    }

    m_hooked_get_primary_camera = true;

    const auto sceneview_t = sdk::find_type_definition("via.SceneView");
    const auto get_primary_camera_fn = sceneview_t->get_method("get_PrimaryCamera");
    REVR_LOG(spdlog::info("Hooking getPrimaryCamera: {:x}", (uintptr_t)get_primary_camera_fn));

    g_hookman.add(get_primary_camera_fn, [this](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>& arg_tys, uintptr_t ret_addr) {
        quadview::State::get().calls_primary_camera.fetch_add(1, std::memory_order_relaxed); // [CPU 22.10.2026]

        if (m_new_cameras.empty()) {
            return HookManager::PreHookResult::CALL_ORIGINAL;
        }

        std::scoped_lock _{ m_camera_mutex };

        for (auto new_camera : m_new_cameras) {
            //*(int32_t*)((uint8_t*)new_camera + PRIORITY_OFFSET) = *(int32_t*)((uint8_t*)get_old_camera_counterpart(new_camera) + PRIORITY_OFFSET) + 1; // causes the game to NOT use the new camera as the main camera
            *(int32_t*)((uint8_t*)new_camera + PRIORITY_OFFSET) = -1;
            *(int32_t*)((uint8_t*)new_camera + PRIORITY_OFFSET + 8) = (int32_t)via::CameraType::Debug;
        }

        return HookManager::PreHookResult::CALL_ORIGINAL;
    },
    [this](uintptr_t& ret_val, sdk::RETypeDefinition* ret_ty, uintptr_t ret_addr) {
        std::scoped_lock _{ m_camera_mutex };

        for (auto new_camera : m_new_cameras) {
            *(int32_t*)((uint8_t*)new_camera + PRIORITY_OFFSET + 8) = (int32_t)via::CameraType::Game;
        }
    });
}


void CameraDuplicator::copy_camera_properties(size_t wanted) {
#if TDB_VER >= 69
    if (!m_copy_camera) {
        return;
    }

    const auto old_camera = sdk::get_primary_camera();

    if (old_camera == nullptr) {
        return;
    }

    std::scoped_lock _{ m_camera_mutex };

    auto clones_it = m_old_to_new_cameras.find((RECamera*)old_camera);

    if (clones_it == m_old_to_new_cameras.end() || clones_it->second.empty()) {
        return;
    }

    const auto old_camera_gameobject = utility::re_component::get_game_object((REComponent*)old_camera);

    if (old_camera_gameobject == nullptr) {
        return;
    }

    wanted = (std::min)(wanted, MAX_CLONES);

    // Only the clones of the current primary camera draw, and only as many as the technique wants
    for (auto camera : m_new_cameras) {
        const auto camera_gameobject = utility::re_component::get_game_object((REComponent*)camera);

        if (camera_gameobject == nullptr) {
            continue;
        }

        bool draw = false;

        for (size_t i = 0; i < clones_it->second.size() && i < wanted; ++i) {
            if (clones_it->second[i] == camera) {
                draw = true;
                break;
            }
        }

        camera_gameobject->shouldDraw = draw;
        camera_gameobject->shouldUpdate = false;
    }

    // If a new camera is created, we need to reset the property jobs as components may be different
    if ((RECamera*)old_camera != m_last_primary_camera) {
        m_getter_setters.clear();
        m_property_jobs.clear();
    }

    m_last_primary_camera = (RECamera*)old_camera;

    static auto via_camera = sdk::find_type_definition("via.Camera");

    static auto get_near_clip_plane_method = via_camera->get_method("get_NearClipPlane");
    static auto set_near_clip_plane_method = via_camera->get_method("set_NearClipPlane");
    static auto get_far_clip_plane_method = via_camera->get_method("get_FarClipPlane");
    static auto set_far_clip_plane_method = via_camera->get_method("set_FarClipPlane");
    static auto get_fov_method = via_camera->get_method("get_FOV");
    static auto set_fov_method = via_camera->get_method("set_FOV");
    static auto get_vertical_enable_method = via_camera->get_method("get_VerticalEnable");
    static auto set_vertical_enable_method = via_camera->get_method("set_VerticalEnable");
    static auto set_aspect_ratio_method = via_camera->get_method("set_AspectRatio");
    static auto get_aspect_ratio_method = via_camera->get_method("get_AspectRatio");

    auto ctx = sdk::get_thread_context();

    for (size_t clone_index = 0; clone_index < clones_it->second.size() && clone_index < wanted; ++clone_index) {
        auto new_camera = clones_it->second[clone_index];

        if (new_camera == nullptr) {
            continue;
        }

        const auto new_camera_gameobject = utility::re_component::get_game_object((REComponent*)new_camera);

        if (new_camera_gameobject == nullptr) {
            continue;
        }

        // Do not allow the camera components to update. We will do the update ourselves via the copying of properties
        new_camera_gameobject->shouldUpdate = false;

        if (get_near_clip_plane_method != nullptr && set_near_clip_plane_method != nullptr) {
            const auto old_near_clip_plane = get_near_clip_plane_method->call<float>(ctx, old_camera);
            set_near_clip_plane_method->call<void>(ctx, new_camera, old_near_clip_plane);
        }

        if (get_far_clip_plane_method != nullptr && set_far_clip_plane_method != nullptr) {
            const auto old_far_clip_plane = get_far_clip_plane_method->call<float>(ctx, old_camera);
            set_far_clip_plane_method->call<void>(ctx, new_camera, old_far_clip_plane);
        }

        if (get_fov_method != nullptr && set_fov_method != nullptr) {
            const auto old_fov = get_fov_method->call<float>(ctx, old_camera);
            set_fov_method->call<void>(ctx, new_camera, old_fov);
        }

        if (get_vertical_enable_method != nullptr && set_vertical_enable_method != nullptr) {
            const auto old_vertical_enable = get_vertical_enable_method->call<bool>(ctx, old_camera);
            set_vertical_enable_method->call<void>(ctx, new_camera, old_vertical_enable);
        }

        if (get_aspect_ratio_method != nullptr && set_aspect_ratio_method != nullptr) {
            const auto old_aspect_ratio = get_aspect_ratio_method->call<float>(ctx, old_camera);
            set_aspect_ratio_method->call<void>(ctx, new_camera, old_aspect_ratio);
        }

        auto& jobs = m_property_jobs[new_camera];

        for (const auto& descriptor : m_wanted_components) {
            if (!descriptor.allowed) {
                continue;
            }

            const auto old_component = utility::re_component::find<REComponent>(old_camera_gameobject->transform, descriptor.name);
            const auto new_component = utility::re_component::find<REComponent>(new_camera_gameobject->transform, descriptor.name);

            if (old_component == nullptr || new_component == nullptr) {
                continue;
            }

            const auto t1 = utility::re_managed_object::get_type_definition(old_component);
            const auto t2 = utility::re_managed_object::get_type_definition(new_component);

            const auto t_name_fnv = utility::hash(t1->get_full_name());

            // getter/setter cache per type (shared by all clones)
            if (!m_getter_setters.contains(t1)) {
                const auto methods = t1->get_methods();

                for (auto& method : methods) {
                    const auto name = std::string{method.get_name()};

                    if (name.find("get_") == 0) {
                        REVR_LOG(spdlog::info("Found getter {}", name));

                        const auto name_front = name.substr(4);
                        m_getter_setters[t1][name_front].getter = &method;
                    } else if (name.find("set_") == 0) {
                        REVR_LOG(spdlog::info("Found setter {}", name));

                        const auto name_front = name.substr(4);
                        m_getter_setters[t1][name_front].setter = &method;
                    }
                }
            }

            // Add property jobs for this clone (once per type and clone); the copying is spread out over multiple
            // frames as it is a lot of work
            if (!jobs.types_added.contains(t1)) {
                jobs.types_added.insert(t1);

                for (const auto& [name, methods] : m_getter_setters[t1]) {
                    if (methods.getter == nullptr || methods.setter == nullptr) {
                        continue;
                    }

                    const auto result_type = methods.getter->get_return_type();
                    const auto should_pass_result_ptr = result_type != nullptr && result_type->is_value_type() && (result_type->get_valuetype_size() > sizeof(void*) || (!result_type->is_primitive() && !result_type->is_enum()));

                    jobs.jobs.push_back([allowed = &descriptor.allowed, old_component, new_component, getter = methods.getter, setter = methods.setter, result_type, should_pass_result_ptr]() {
                        if (!*allowed) {
                            return;
                        }

                        const auto result = getter->invoke(old_component, {});

                        if (result_type == nullptr) {
                            setter->invoke(new_component, result.ptr);
                        } else {
                            if (should_pass_result_ptr) {
                                setter->invoke(new_component, (void*)result.bytes.data());
                            } else {
                                const auto current_value = getter->invoke(new_component, {});

                                if (current_value.ptr != result.ptr) {
                                    setter->invoke(new_component, result.ptr);
                                }
                            }
                        }
                    });
                }
            }

            // Properties that need to be manually set
            if (t_name_fnv == "via.render.LDRPostProcess"_fnv) {
                static auto t = t1;
                static auto get_color_correct = t->get_method("get_ColorCorrect");

                if (get_color_correct != nullptr) {
                    const auto color_correct_old = get_color_correct->call<::REManagedObject*>(ctx, old_component);
                    const auto color_correct_new = get_color_correct->call<::REManagedObject*>(ctx, new_component);

                    if (color_correct_old != nullptr && color_correct_new != nullptr) {
                        static auto color_t = utility::re_managed_object::get_type_definition(color_correct_old);
                        static auto get_enabled = color_t->get_method("get_Enabled");

                        if (get_enabled != nullptr) {
                            const auto enabled_old = get_enabled->call<bool>(ctx, color_correct_old);
                            const auto enabled_new = get_enabled->call<bool>(ctx, color_correct_new);

                            if (enabled_old != enabled_new) {
                                static auto set_enabled = color_t->get_method("set_Enabled");
                                set_enabled->call(ctx, color_correct_new, enabled_old);
                            }
                        }

                        // [COLOUR CORRECTION 02.11.2026] (test) all its values, not only Enabled
                        if (m_copy_color_correct) {
#if REVR_RELEASE // [RELEASE SWITCH fix141] without the names for the log
                            copy_object_properties(color_correct_old, color_correct_new, nullptr);
#else
                            std::string names{};
                            const auto n = copy_object_properties(color_correct_old, color_correct_new, m_color_copy_logged ? nullptr : &names);

                            if (!m_color_copy_logged) {
                                m_color_copy_logged = true;
                                REVR_LOG(spdlog::info("[CameraDuplicator] colour correction: {} properties copied into the clone every frame: {}", n, names));
                            }
#endif
                        }
                    }
                }
            } else if (t_name_fnv == "via.render.SoftBloom"_fnv) {
                static auto t = t1;
                static auto get_algorithm = t->get_method("getAlgorithm");
                static auto set_algorithm = t->get_method("setAlgorithm");

                if (get_algorithm != nullptr && set_algorithm != nullptr) {
                    const auto algorithm_old = get_algorithm->call<uint32_t>(ctx, old_component);
                    const auto algorithm_new = get_algorithm->call<uint32_t>(ctx, new_component);

                    if (algorithm_old != algorithm_new) {
                        set_algorithm->call(ctx, new_component, algorithm_old);
                    }
                }

                static auto get_dirt_mask_blend = t->get_method("getDirtMaskBlend");
                static auto set_dirt_mask_blend = t->get_method("setDirtMaskBlend");

                if (get_dirt_mask_blend != nullptr && set_dirt_mask_blend != nullptr) {
                    const auto dirt_mask_old = get_dirt_mask_blend->call<void*>(ctx, old_component);
                    const auto dirt_mask_new = get_dirt_mask_blend->call<void*>(ctx, new_component);

                    if (dirt_mask_old != dirt_mask_new) {
                        set_dirt_mask_blend->call(ctx, new_component, dirt_mask_old);
                    }
                }

                static auto get_size_scale_count = t->get_method("getSizeScaleCount");
                static auto set_size_scale_count = t->get_method("setSizeScaleCount");

                if (get_size_scale_count != nullptr && set_size_scale_count != nullptr) {
                    const auto size_scale_count_old = get_size_scale_count->call<int>(sdk::get_thread_context(), old_component);
                    const auto size_scale_count_new = get_size_scale_count->call<int>(sdk::get_thread_context(), new_component);

                    if (size_scale_count_old != size_scale_count_new) {
                        set_size_scale_count->call(ctx, new_component, size_scale_count_old);

                        static auto get_size_scale = t->get_method("getSizeScale");
                        static auto set_size_scale = t->get_method("setSizeScale");

                        if (get_size_scale != nullptr && set_size_scale != nullptr) {
                            for (auto i = 0; i < size_scale_count_old; i++) {
                                const auto size_scale_old = get_size_scale->call<float>(ctx, old_component, i);
                                const auto size_scale_new = get_size_scale->call<float>(ctx, new_component, i);

                                if (size_scale_old != size_scale_new) {
                                    set_size_scale->call(ctx, new_component, i, size_scale_old);
                                }
                            }
                        }
                    }
                }
            } else if (t_name_fnv == "via.render.ToneMapping"_fnv) {
                static auto t = t1;
                static auto get_vignetting = t->get_method("getVignetting");
                static auto set_vignetting = t->get_method("setVignetting");

                if (get_vignetting != nullptr && set_vignetting != nullptr) {
                    const auto vignetting_old = get_vignetting->call<uint32_t>(ctx, old_component);
                    const auto vignetting_new = get_vignetting->call<uint32_t>(ctx, new_component);
                    const auto is_disable_vignette = Camera::get()->is_disable_vignette();

                    if (!is_disable_vignette) {
                        if (vignetting_old != vignetting_new) {
                            set_vignetting->call(ctx, new_component, vignetting_old);
                        }
                    } else {
                        set_vignetting->call(ctx, new_component, via::render::ToneMapping::Vignetting::Disable);
                    }
                }

                static auto get_auto_exposure = t->get_method("getAutoExposure");
                static auto set_auto_exposure = t->get_method("setAutoExposure");

                if (get_auto_exposure != nullptr && set_auto_exposure != nullptr) {
                    const auto auto_exposure_old = get_auto_exposure->call<uint32_t>(ctx, old_component);
                    const auto auto_exposure_new = get_auto_exposure->call<uint32_t>(ctx, new_component);

                    if (auto_exposure_old != auto_exposure_new) {
                        set_auto_exposure->call(ctx, new_component, auto_exposure_old);
                    }
                }

                static auto get_neighborhood_clamp = t->get_method("getNeighborhoodClamp");
                static auto set_neighborhood_clamp = t->get_method("setNeighborhoodClamp");

                if (get_neighborhood_clamp != nullptr && set_neighborhood_clamp != nullptr) {
                    const auto neighborhood_clamp_old = get_neighborhood_clamp->call<uint32_t>(ctx, old_component);
                    const auto neighborhood_clamp_new = get_neighborhood_clamp->call<uint32_t>(ctx, new_component);

                    if (neighborhood_clamp_old != neighborhood_clamp_new) {
                        set_neighborhood_clamp->call(ctx, new_component, neighborhood_clamp_old);
                    }
                }

                static auto get_temporal_aa = t->get_method("getTemporalAA");
                static auto set_temporal_aa = t->get_method("setTemporalAA");

                if (get_temporal_aa != nullptr && set_temporal_aa != nullptr) {
                    const auto temporal_aa_old = get_temporal_aa->call<uint32_t>(ctx, old_component);
                    const auto temporal_aa_new = get_temporal_aa->call<uint32_t>(ctx, new_component);

                    if (temporal_aa_old != temporal_aa_new) {
                        set_temporal_aa->call(ctx, new_component, temporal_aa_old);
                    }
                }

                static auto get_temporal_aa_algorithm =  t->get_method("getTemporalAAAlgorithm");
                static auto set_temporal_aa_algorithm =  t->get_method("setTemporalAAAlgorithm");

                if (get_temporal_aa_algorithm != nullptr && set_temporal_aa_algorithm != nullptr) {
                    const auto temporal_aa_algorithm_old = get_temporal_aa_algorithm->call<uint32_t>(ctx, old_component);
                    const auto temporal_aa_algorithm_new = get_temporal_aa_algorithm->call<uint32_t>(ctx, new_component);

                    if (temporal_aa_algorithm_old != temporal_aa_algorithm_new) {
                        set_temporal_aa_algorithm->call(ctx, new_component, temporal_aa_algorithm_old);
                    }
                }
            }
        }

        // [CONTACT SHADOWS 10.11.2026] via.render.RenderOutput.Cutscene of the game camera into the clone, every frame (the
        // RenderOutput is not among the copied components: its RenderOutputID is the clone's own). RE2: the game camera's
        // RenderOutput has Cutscene true, a newly created one false, and with false the engine leaves the contact shadows
        // (graphics option "Contact Shadows") out of that camera's view: the small shadows under objects were missing in
        // the quad view's fovea views and in the second eye of the plain multipass (test of 10.11.2026: back in both with
        // the value copied).
        {
            static const auto render_output_t = sdk::find_type_definition("via.render.RenderOutput");
            static const auto get_cutscene = render_output_t != nullptr ? render_output_t->get_method("get_Cutscene") : nullptr;
            static const auto set_cutscene = render_output_t != nullptr ? render_output_t->get_method("set_Cutscene") : nullptr;

            if (get_cutscene != nullptr && set_cutscene != nullptr) {
                const auto old_output = utility::re_component::find<REComponent>(old_camera_gameobject->transform, "via.render.RenderOutput");
                const auto new_output = utility::re_component::find<REComponent>(new_camera_gameobject->transform, "via.render.RenderOutput");

                if (old_output != nullptr && new_output != nullptr) {
                    const auto cutscene = get_cutscene->call<bool>(ctx, old_output);

                    if (get_cutscene->call<bool>(ctx, new_output) != cutscene) {
                        set_cutscene->call<void>(ctx, new_output, cutscene);
                    }
                }
            }
        }

        // Run the jobs in segments: 25 per clone and frame, the index wraps around
        constexpr size_t jobs_per_segment = 25;

        if (jobs.index >= jobs.jobs.size()) {
            jobs.index = 0;
        }

        const auto jobs_to_run = (std::min)(jobs_per_segment, jobs.jobs.size() - jobs.index);

        for (size_t i = 0; i < jobs_to_run; i++) {
            jobs.jobs[jobs.index + i]();
        }

        jobs.index += jobs_to_run;

        if (jobs.index >= jobs.jobs.size()) {
            jobs.index = 0;
        }
    }

    // [EXPOSURE TEST 01.11.2026] after the copies: the test's tone mapping values win in this frame
    apply_tone_test(ctx, old_camera_gameobject, clones_it->second, wanted);
    apply_view_scale(ctx, old_camera_gameobject, clones_it->second, wanted); // [VIEW SCALE 15.11.2026]
    apply_screen_effects(ctx, old_camera_gameobject, clones_it->second, wanted); // [SCREEN EFFECTS 15.11.2026]
    apply_image_quality(ctx, old_camera_gameobject, clones_it->second, wanted); // [IMAGE QUALITY 15.11.2026]
#endif
}

// [IMAGE QUALITY 15.11.2026] RE7, the video tape with another character (dumps of 15.11.2026 15:24, 16:01 and 16:29): the
// periphery was off the fovea, in each eye the other way, more towards the bottom of the picture (it looked tilted). There
// the game camera is drawn at 2/3 of the size: its depth and motion 746x576 while its colour comes out at 1120x864 (the
// game's own upscale), most likely through via.render.RenderOutput ImageQuality. The reprojection to the eyes and the
// periphery's TAA read the depth and the motion at the colour's pixels: the depth of other places (and none past 746 x 576),
// so each eye's periphery moved by a wrong parallax. The shaders now read them at the colour's place whatever their size
// (FoveaComposite.cpp, wide_depth_at); here, while the fovea views are drawn, the game camera gets the clones' value (the
// wide view at the size the quad view laid out).
// The property's type comes from the type database. The camera check printed it as 2 / 0, but that check's invoke returns
// floats wrongly in this game (ViewScale 3.7e19 / -2 for 0.35 / 0.45), so it said nothing about the values; the fix of 16:00
// read it with call<int32_t>, which for a float getter returns a leftover register (the same twice: nothing was set).
// (not put back afterwards: the game sets its value when a scene wants it, and a value put back by us later could be one
// the game no longer wants; without the quad view the only difference is the wide view's size)
void CameraDuplicator::apply_image_quality(void* ctx, REGameObject* main_gameobject, const std::vector<RECamera*>& clones, size_t wanted) {
    static const auto t = sdk::find_type_definition("via.render.RenderOutput");
    static const auto get_m = t != nullptr ? t->get_method("get_ImageQuality") : nullptr;
    static const auto set_m = t != nullptr ? t->get_method("set_ImageQuality") : nullptr;
    static const auto rt = get_m != nullptr ? get_m->get_return_type() : nullptr;
    static const std::string rt_name = rt != nullptr ? std::string{rt->get_full_name()} : std::string{"(none)"};
    static const bool is_float = rt_name == "System.Single";
    static const bool is_int32 = !is_float && rt != nullptr && (rt->is_enum() || rt->is_primitive()) && rt->get_valuetype_size() == 4;
    static bool type_logged = false;

    if (!type_logged) {
        type_logged = true;
        REVR_LOG(spdlog::info("[CameraDuplicator] image quality: via.render.RenderOutput get_ImageQuality {}, set_ImageQuality {}, type {}{}", get_m != nullptr,
            set_m != nullptr, rt_name, is_float || is_int32 ? "" : " (not held: unknown type)"));
    }

    if (get_m == nullptr || set_m == nullptr || !(is_float || is_int32) || main_gameobject == nullptr || main_gameobject->transform == nullptr) {
        return;
    }

    if (wanted == 0 || clones.empty() || clones[0] == nullptr || m_image_quality_given_up ||
        !quadview::State::get().image_quality_hold.load(std::memory_order_relaxed)) {
        return;
    }

    auto* main_comp = utility::re_component::find<REComponent>(main_gameobject->transform, "via.render.RenderOutput");
    auto clone_go = utility::re_component::get_game_object((REComponent*)clones[0]);
    auto* clone_comp = clone_go != nullptr && clone_go->transform != nullptr ? utility::re_component::find<REComponent>(clone_go->transform, "via.render.RenderOutput")
                                                                              : nullptr;

    if (main_comp == nullptr || clone_comp == nullptr) {
        return;
    }

    const auto read = [&](REComponent* c) -> double {
        return is_float ? (double)get_m->call<float>(ctx, c) : (double)get_m->call<int32_t>(ctx, c);
    };

    const double target = read(clone_comp);
    const double now = read(main_comp);
    const bool differs = std::abs(now - target) > 1e-4;

    // the values once at the start, then each time the game camera gets a value of its own
    if (m_image_quality_logged < 20 && (m_image_quality_logged == 0 || (differs && std::abs(now - m_image_quality_last_logged) > 1e-4))) {
        ++m_image_quality_logged;
        m_image_quality_last_logged = now;
        REVR_LOG(spdlog::info("[CameraDuplicator] image quality: game camera {:.4g}, fovea views {:.4g}{}", now, target,
            differs ? " -> the game camera set to the fovea views' value while they are drawn" : " (the same)"));
    }

    // a guard: if the game puts its value back nearly every frame, it is left to the game (the shaders read the depth at
    // the right place anyway) instead of the two values taking turns
    if (++m_image_quality_window_calls >= 600) {
        m_image_quality_window_calls = 0;
        m_image_quality_window_sets = 0;
    }

    if (!differs || !(target > 0.0)) {
        return;
    }

    if (++m_image_quality_window_sets > 120) {
        m_image_quality_given_up = true;
        REVR_LOG(spdlog::warn("[CameraDuplicator] image quality: the game set its value {:.4g} back {} times within {} frames: left to the game from now on", now,
            m_image_quality_window_sets - 1, m_image_quality_window_calls));
        return;
    }

    if (is_float) {
        set_m->call<void*>(ctx, main_comp, (float)target);
    } else {
        set_m->call<void*>(ctx, main_comp, (int32_t)(target >= 0.0 ? target + 0.5 : target - 0.5));
    }
}

// [SCREEN EFFECTS 15.11.2026] RE7 dumps of 15.11.2026 15:24 (the video tape with another character): the periphery did not
// match the fovea, in the left eye shifted one way and in the right eye the other (13-17 eye pixels; 0 in the house). There
// the game camera and its clones have via.render.PolynomialLens and via.render.RetroFilm on (off in the house): a lens
// distortion and a video tape look made in each view's own image, around its own centre and at its own pixel size, which
// differ between the wide view and the fovea views, and which the reprojection to the eyes does not know about. Both off on
// every camera of the quad view while it is drawn; the ones switched off here are switched on again otherwise.
void CameraDuplicator::apply_screen_effects(void* ctx, REGameObject* main_gameobject, const std::vector<RECamera*>& clones, size_t wanted) {
    static constexpr const char* NAMES[2] = {"via.render.PolynomialLens", "via.render.RetroFilm"};
    const bool want = wanted > 0 && quadview::State::get().screen_effects_off.load(std::memory_order_relaxed);

    const auto set_enabled = [&](REComponent* c, bool on) {
        const auto t = utility::re_managed_object::get_type_definition(c);
        const auto set_m = t != nullptr ? t->get_method("set_Enabled") : nullptr;

        if (set_m != nullptr) {
            set_m->call<void*>(ctx, c, on);
        }
    };

    if (!want) {
        if (!m_screen_effects_off.empty()) {
            for (auto* c : m_screen_effects_off) {
                set_enabled(c, true);
                utility::re_managed_object::release((::REManagedObject*)c); // held since it was switched off
            }

            REVR_LOG(spdlog::info("[CameraDuplicator] screen effects: {} component(s) switched on again", m_screen_effects_off.size()));
            m_screen_effects_off.clear();
        }

        return;
    }

    std::vector<REGameObject*> objects{main_gameobject};

    for (size_t i = 0; i < (std::min)(wanted, clones.size()); ++i) {
        if (clones[i] != nullptr) {
            objects.push_back(utility::re_component::get_game_object((REComponent*)clones[i]));
        }
    }

    for (auto* go : objects) {
        if (go == nullptr || go->transform == nullptr) {
            continue;
        }

        for (const auto* name : NAMES) {
            auto* c = utility::re_component::find<REComponent>(go->transform, name);

            if (c == nullptr) {
                continue;
            }

            const auto t = utility::re_managed_object::get_type_definition(c);
            const auto get_m = t != nullptr ? t->get_method("get_Enabled") : nullptr;

            if (get_m == nullptr || !get_m->call<bool>(ctx, c)) {
                continue;
            }

            set_enabled(c, false);

            // held (the scene may unload the camera before it is switched on again)
            if (std::find(m_screen_effects_off.begin(), m_screen_effects_off.end(), c) == m_screen_effects_off.end()) {
                utility::re_managed_object::add_ref((::REManagedObject*)c);
                m_screen_effects_off.push_back(c);
            }

            if (m_screen_effects_logged < 20) {
                ++m_screen_effects_logged;
                REVR_LOG(spdlog::info("[CameraDuplicator] screen effects: {} on {} switched off while the fovea views are drawn", name,
                    utility::re_string::get_string(go->name)));
            }
        }
    }
}

// [VIEW SCALE 15.11.2026] RE7 dumps of 15.11.2026 (Mia's photo, another photo looked at): the object looked at had a doubled
// edge at the fovea's border. Its depth said 0.09 m (raw 0.558 with the near plane 0.05) while the two fovea views saw it
// 0.2 m away (their disparity, and the wide view's position half way between them): the engine draws it with view space
// scaling (via.render.Mesh ViewSpaceScaling, the factor in the camera's via.render.ViewScaling), which moves it towards
// each camera and changes its depth, not its picture. The periphery, reprojected from the wide view with that depth, moved
// it by 0.25 instead of 0.16 (tangent) and showed it a second time in the fovea's blend band. With the factor at 1 the
// depth is the object's own (the picture of each view stays the same; only a wall nearer than the object would cut it).
// While the fovea views are drawn and the option is on; the game camera's own value is put back otherwise.
void CameraDuplicator::apply_view_scale(void* ctx, REGameObject* main_gameobject, const std::vector<RECamera*>& clones, size_t wanted) {
    static const auto t = sdk::find_type_definition("via.render.ViewScaling");
    static const auto get_m = t != nullptr ? t->get_method("get_ViewScale") : nullptr;
    static const auto set_m = t != nullptr ? t->get_method("set_ViewScale") : nullptr;
    static bool missing_logged = false;

    if (get_m == nullptr || set_m == nullptr) {
        if (!missing_logged) {
            missing_logged = true;
            REVR_LOG(spdlog::info("[CameraDuplicator] view scaling: via.render.ViewScaling get/set_ViewScale not found (type {})", t != nullptr));
        }

        return;
    }

    if (main_gameobject == nullptr || main_gameobject->transform == nullptr) {
        return;
    }

    auto* main_comp = utility::re_component::find<REComponent>(main_gameobject->transform, "via.render.ViewScaling");

    if (main_comp == nullptr) {
        return;
    }

    const auto clone_comp = [&](size_t i) -> REComponent* {
        auto go = i < clones.size() && clones[i] != nullptr ? utility::re_component::get_game_object((REComponent*)clones[i]) : nullptr;
        return go != nullptr && go->transform != nullptr ? utility::re_component::find<REComponent>(go->transform, "via.render.ViewScaling") : nullptr;
    };

    const bool want = wanted > 0 && quadview::State::get().real_view_scale.load(std::memory_order_relaxed);
    const float now = get_m->call<float>(ctx, main_comp);

    if (want) {
        if (!m_view_scale_saved) {
            m_view_scale_original = now;
            m_view_scale_saved = true;
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
            std::string clone_values{};

            for (size_t i = 0; i < (std::min)(wanted, clones.size()); ++i) {
                auto* c = clone_comp(i);
                clone_values += std::format("{}{}", clone_values.empty() ? "" : ", ", c != nullptr ? std::format("{:.3f}", get_m->call<float>(ctx, c)) : "none");
            }

            REVR_LOG(spdlog::info("[CameraDuplicator] view scaling: game camera ViewScale {:.3f}, fovea cameras {} -> 1 while the fovea views are drawn", now,
                clone_values));
#endif
        } else if (std::abs(now - 1.0f) > 1e-4f) {
            // the game set its own value again (e.g. when an object is taken up): kept for the restore
            if (m_view_scale_changes_logged < 20) {
                ++m_view_scale_changes_logged;
                REVR_LOG(spdlog::info("[CameraDuplicator] view scaling: the game set ViewScale {:.3f} (was {:.3f}), held at 1", now, m_view_scale_original));
            }

            m_view_scale_original = now;
        }

        if (std::abs(now - 1.0f) > 1e-4f) {
            set_m->call<void*>(ctx, main_comp, 1.0f);
        }

        for (size_t i = 0; i < (std::min)(wanted, clones.size()); ++i) {
            if (auto* c = clone_comp(i); c != nullptr && std::abs(get_m->call<float>(ctx, c) - 1.0f) > 1e-4f) {
                set_m->call<void*>(ctx, c, 1.0f);
            }
        }
    } else if (m_view_scale_saved) {
        m_view_scale_saved = false;
        set_m->call<void*>(ctx, main_comp, m_view_scale_original);

        for (size_t i = 0; i < clones.size(); ++i) {
            if (auto* c = clone_comp(i); c != nullptr) {
                set_m->call<void*>(ctx, c, m_view_scale_original);
            }
        }

        REVR_LOG(spdlog::info("[CameraDuplicator] view scaling: ViewScale {:.3f} back", m_view_scale_original));
    }
}

// [COLOUR CORRECTION 02.11.2026] see set_copy_color_correct. Values (and structures) through their bytes as the property
// jobs pass them; objects by reference (the clone then uses the same object, e.g. the same colour table)
size_t CameraDuplicator::copy_object_properties(::REManagedObject* src, ::REManagedObject* dst, std::string* names) {
    using Pair = std::pair<sdk::REMethodDefinition*, sdk::REMethodDefinition*>;
    static std::unordered_map<sdk::RETypeDefinition*, std::vector<Pair>> cache{};

    auto* t = src != nullptr ? utility::re_managed_object::get_type_definition(src) : nullptr;

    if (t == nullptr || dst == nullptr || utility::re_managed_object::get_type_definition(dst) != t) {
        return 0;
    }

    auto it = cache.find(t);

    if (it == cache.end()) {
        std::unordered_map<std::string, sdk::REMethodDefinition*> setters{};
        std::vector<Pair> pairs{};

        for (auto& m : t->get_methods()) {
            if (m.get_name() == nullptr || m.get_num_params() != 1 || m.is_static()) {
                continue;
            }

            const std::string n = m.get_name();

            if (n.starts_with("set_") || (n.size() > 3 && n.starts_with("set") && std::isupper((unsigned char)n[3]))) {
                setters[n] = &m;
            }
        }

        for (auto& m : t->get_methods()) {
            if (m.get_name() == nullptr || m.get_num_params() != 0 || m.is_static()) {
                continue;
            }

            const std::string n = m.get_name();
            std::string setter_name{};

            if (n.starts_with("get_")) {
                setter_name = "set_" + n.substr(4);
            } else if (n.size() > 3 && n.starts_with("get") && std::isupper((unsigned char)n[3])) {
                setter_name = "set" + n.substr(3);
            } else {
                continue;
            }

            if (const auto s = setters.find(setter_name); s != setters.end()) {
                pairs.emplace_back(&m, s->second);
            }
        }

        it = cache.emplace(t, std::move(pairs)).first;
    }

    size_t count = 0;

    for (const auto& [getter, setter] : it->second) {
        const auto* rt = getter->get_return_type();

        if (rt == nullptr) {
            continue;
        }

        const bool by_ptr = rt->is_value_type() && (rt->get_valuetype_size() > sizeof(void*) || (!rt->is_primitive() && !rt->is_enum()));
        auto r = getter->invoke(src, std::span<void*>{});

        if (r.exception_thrown) {
            continue;
        }

        if (by_ptr) {
            setter->invoke(dst, (void*)r.bytes.data());
        } else {
            setter->invoke(dst, r.ptr);
        }

        ++count;

        if (names != nullptr) {
            *names += (names->empty() ? "" : ", ") + std::string{getter->get_name()};
        }
    }

    return count;
}

// [FOVEA RAY TRACING 12.11.2026] With the game's ray tracing on (RE7 / RE2 ray tracing versions) the game camera has a
// via.render.ExperimentalRayTrace component. The clones were made without it (illegal_components in clone_camera) and the
// game's hikako.raytracing.RayTracingController does not run on them (their game object is not updated), so the fovea
// views were rendered without the ray-traced light and reflections: cooler and flatter than the wide view, with the
// screen-space reflections instead (RE7 dumps of 12.11.2026). Each clone that draws gets a component of its own, with the
// game camera's values (every value-type property that differs, Enabled included). Runs in LockScene, like the creation
// of the clones.
// [RT LIFETIME 12.11.2026] RE7 tests of 12.11.2026:
// - 21:38: the clones' components, made in game, stayed enabled (their clones no longer drawing) through two minutes of
//   main menu and a new game; the first frames of the new game read a null array in a mesh draw (re7.exe + 0x2E797E7,
//   its object next to the memory of clone 1's component);
// - 22:45: same crash entering a new game, with the components destroyed in the main menu and made again 90 frames into
//   the game (fix33);
// - 22:48: the game stopped presenting frames right after the components were destroyed (option switched off and on a
//   few times in game, each time enable + destroy in the same frame);
// - 13.11.2026 12:03: the same, switching from the quad view to the plain multipass with the game's ray tracing on: the
//   game stopped right after clone 1's component was destroyed (60 frames after it was switched off). The one destroy
//   that went well (11:52:46) came with the game camera without ray tracing (main menu).
// So now:
// - a component is switched off as soon as it is not in use (clone not drawing, camera no longer the game's camera, game
//   camera's component off or replaced, option off); it is destroyed only while the game camera has no ray tracing on
//   (main menu, ray tracing off in the game's options) and after RT_DESTROY_FRAMES frames switched off; otherwise it
//   stays, switched off, and is reused;
// - a component is made, or switched on again, only after the game camera's component and the clone have both been in
//   use for RT_WARMUP_FRAMES frames in a row (not while a level or a camera change is still loading).
void CameraDuplicator::sync_clone_ray_tracing(size_t wanted) {
#if TDB_VER >= 69 && !defined(RE4)
    constexpr uint32_t RT_WARMUP_FRAMES = 300;
    constexpr uint32_t RT_DESTROY_FRAMES = 60;

    static const auto rt_t = sdk::find_type_definition("via.render.ExperimentalRayTrace");

    if (rt_t == nullptr) {
        return;
    }

    static const auto get_enabled = rt_t->get_method("get_Enabled");
    static const auto set_enabled = rt_t->get_method("set_Enabled");
    static const auto get_mode = rt_t->get_method("get_RaytracingMode");
    static const auto gameobject_t = sdk::find_type_definition("via.GameObject");
    static const auto create_component_fn = gameobject_t != nullptr ? gameobject_t->get_method("createComponent(System.Type)") : nullptr;

    if (get_enabled == nullptr || set_enabled == nullptr || create_component_fn == nullptr) {
        return;
    }

    auto ctx = sdk::get_thread_context();
    const auto primary = (RECamera*)sdk::get_primary_camera();
    const bool option = quadview::State::get().fovea_ray_tracing.load(std::memory_order_relaxed);

    // the game camera's component and how long it has been on (the same component)
    REComponent* old_rt = nullptr;
    bool game_on = false;

    if (primary != nullptr) {
        if (const auto old_go = utility::re_component::get_game_object((REComponent*)primary); old_go != nullptr && old_go->transform != nullptr) {
            old_rt = utility::re_component::find<REComponent>(old_go->transform, rt_t->get_type());
            game_on = old_rt != nullptr && get_enabled->call<bool>(ctx, old_rt);
        }
    }

    const bool game_use = game_on && option;
    const bool game_changed = old_rt != m_rt_game_component;

    if (game_use && !game_changed) {
        ++m_rt_game_frames;
    } else {
        m_rt_game_frames = game_use ? 1 : 0;
    }

    m_rt_game_component = game_use ? old_rt : nullptr;

    std::scoped_lock _{ m_camera_mutex };

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    std::string state = std::format("option {}, game camera {}", option ? "on" : "off",
        old_rt == nullptr ? "without it" : game_on ? "on" : "off");

    if (game_on && get_mode != nullptr) {
        state += std::format(" (mode {})", get_mode->call<int32_t>(ctx, old_rt));
    }
#endif

    for (auto& [original, clones] : m_old_to_new_cameras) {
        for (size_t i = 0; i < clones.size(); ++i) {
            const auto clone = clones[i];

            if (clone == nullptr) {
                continue;
            }

            const auto go = utility::re_component::get_game_object((REComponent*)clone);

            if (go == nullptr || go->transform == nullptr) {
                continue;
            }

            const bool mine = original == primary && i < MAX_CLONES;
            const bool in_use = game_use && !game_changed && mine && i < wanted;
            auto& st = m_rt_clones[clone];
            auto rt = utility::re_component::find<REComponent>(go->transform, rt_t->get_type());

            // the engine removes a destroyed component later: until then it is still listed, and it is not touched
            if (st.destroyed != nullptr && rt != st.destroyed) {
                st.destroyed = nullptr;
            }

            if (rt != nullptr && rt == st.destroyed) {
                st.in_use_frames = 0;

                if (mine) {
                    REVR_LOG(state += std::format(", clone {} removing", i));
                }

                continue;
            }

            if (!in_use) {
                st.in_use_frames = 0;

                if (rt == nullptr) {
                    continue;
                }

                if (get_enabled->call<bool>(ctx, rt)) {
                    set_enabled->call<void>(ctx, rt, false);
                    st.off_frames = 0;
                    REVR_LOG(spdlog::info("[CameraDuplicator] fovea ray tracing: component of clone {} of {} switched off", i,
                        mine ? "the game camera" : "another camera"));
                } else if (++st.off_frames >= RT_DESTROY_FRAMES && !game_on) {
                    st.destroyed = rt;
                    sdk::call_object_func_easy<void*>(rt, "destroy", rt);
                    REVR_LOG(spdlog::info("[CameraDuplicator] fovea ray tracing: component of clone {} of {} destroyed ({} frames off)", i,
                        mine ? "the game camera" : "another camera", st.off_frames));
                }

                if (mine) {
                    REVR_LOG(state += std::format(", clone {} off", i));
                }

                continue;
            }

            ++st.in_use_frames;
            const bool ready = st.in_use_frames >= RT_WARMUP_FRAMES && m_rt_game_frames >= RT_WARMUP_FRAMES;

            if (rt == nullptr) {
                if (!ready) {
                    REVR_LOG(state += std::format(", clone {} waiting", i));
                    continue;
                }

                rt = (REComponent*)create_component_fn->call<sdk::ManagedObject*>(ctx, go, rt_t->get_runtime_type());
                REVR_LOG(spdlog::info("[CameraDuplicator] fovea ray tracing: clone {} got its own via.render.ExperimentalRayTrace @ 0x{:x} "
                             "(game camera's on for {} frames, clone in use for {})", i, (uintptr_t)rt, m_rt_game_frames,
                             st.in_use_frames));

                if (rt == nullptr) {
                    REVR_LOG(state += std::format(", clone {} creation failed", i));
                    continue;
                }

                // made off, switched on below with the game camera's values
                set_enabled->call<void>(ctx, rt, false);
            }

            st.off_frames = 0;
            const bool on = get_enabled->call<bool>(ctx, rt);

            if (!on && !ready) {
                REVR_LOG(state += std::format(", clone {} waiting", i));
                continue;
            }

#if REVR_RELEASE // [RELEASE SWITCH fix141] without the names for the log
            sync_object_values((::REManagedObject*)old_rt, (::REManagedObject*)rt, nullptr, nullptr);
#else
            std::string names{};
            std::string skipped{};
            const bool log_names = !m_rt_names_logged;
            const auto n = sync_object_values((::REManagedObject*)old_rt, (::REManagedObject*)rt, log_names ? &names : nullptr,
                log_names ? &skipped : nullptr);

            if (log_names) {
                m_rt_names_logged = true;
                REVR_LOG(spdlog::info("[CameraDuplicator] fovea ray tracing: {} values of the game camera's component copied into the clones: {} "
                             "| not copied (references): {}", n, names, skipped));
            }
#endif

            if (!get_enabled->call<bool>(ctx, rt)) {
                set_enabled->call<void>(ctx, rt, true);
            }

            REVR_LOG(state += std::format(", clone {} on", i));

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
            if (get_mode != nullptr) {
                state += std::format(" (mode {})", get_mode->call<int32_t>(ctx, rt));
            }
#endif
        }
    }

    // clones that no longer exist
    std::erase_if(m_rt_clones, [this](const auto& p) { return !m_new_cameras.contains(p.first); });

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    // [RT CHECK 12.11.2026] (debug build) a camera check 120 frames after the fovea views got the ray tracing, the clones
    // drawing: the differences between the game camera and the clones while playing (three times per session at most)
    if (state.find(", clone 0 on") != std::string::npos || state.find(", clone 1 on") != std::string::npos) {
        if (++m_rt_on_frames == 120 && m_rt_checks < 3) {
            ++m_rt_checks;
            quadview::State::get().camera_check_request = true;
            REVR_LOG(spdlog::info("[CameraDuplicator] fovea ray tracing: camera check requested (clones on for 120 frames)"));
        }
    } else {
        m_rt_on_frames = 0;
    }

    if (state != m_rt_state) {
        m_rt_state = state;
        REVR_LOG(spdlog::info("[CameraDuplicator] fovea ray tracing: {}", state));
    }
#endif
#endif
}

// [FXAA 12.11.2026] FXAA blurs the whole picture (request of 12.11.2026). The game camera of RE7 has a via.render.FXAA
// component (off with the game's SMAA), the clones got one with the component's default (on: camera check of 12.11).
// With the VR option on, every via.render.FXAA of the game camera and of all clones is kept off, whatever the game's
// anti-aliasing setting says (that one is changed by VR::disable_bad_effects). Runs in LockScene.
void CameraDuplicator::force_fxaa_off() {
#if TDB_VER >= 69
    if (!VR::get()->is_fxaa_forced_off()) {
        return;
    }

    static const auto fxaa_t = sdk::find_type_definition("via.render.FXAA");

    if (fxaa_t == nullptr) {
        return;
    }

    static const auto get_enabled = fxaa_t->get_method("get_Enabled");
    static const auto set_enabled = fxaa_t->get_method("set_Enabled");

    if (get_enabled == nullptr || set_enabled == nullptr) {
        return;
    }

    auto ctx = sdk::get_thread_context();

    const auto switch_off = [&](RECamera* camera, const char* what) {
        if (camera == nullptr) {
            return;
        }

        const auto go = utility::re_component::get_game_object((REComponent*)camera);

        if (go == nullptr || go->transform == nullptr) {
            return;
        }

        const auto fxaa = utility::re_component::find<REComponent>(go->transform, fxaa_t->get_type());

        if (fxaa != nullptr && get_enabled->call<bool>(ctx, fxaa)) {
            set_enabled->call<void>(ctx, fxaa, false);

            if (m_fxaa_switched_off++ < 20) {
                REVR_LOG(spdlog::info("[CameraDuplicator] FXAA switched off on {} {}", what, utility::re_string::get_string(go->name)));
            }
        }
    };

    switch_off(sdk::get_primary_camera(), "the game camera");

    std::scoped_lock _{ m_camera_mutex };

    for (auto camera : m_new_cameras) {
        switch_off(camera, "the clone");
    }
#endif
}

// [FOVEA RAY TRACING 12.11.2026] see sync_clone_ray_tracing. Only value-type properties (numbers, enums, structures):
// a reference may be an object the component owns (as LDRPostProcess.ColorCorrect), which two components must not share.
// Only what differs is written: a setter of the ray tracing may restart its accumulation even with the same value.
size_t CameraDuplicator::sync_object_values(::REManagedObject* src, ::REManagedObject* dst, std::string* names, std::string* skipped) {
    using Pair = std::pair<sdk::REMethodDefinition*, sdk::REMethodDefinition*>;
    static std::unordered_map<sdk::RETypeDefinition*, std::vector<Pair>> cache{};

    auto* t = src != nullptr ? utility::re_managed_object::get_type_definition(src) : nullptr;

    if (t == nullptr || dst == nullptr || utility::re_managed_object::get_type_definition(dst) != t) {
        return 0;
    }

    auto it = cache.find(t);

    if (it == cache.end()) {
        std::unordered_map<std::string, sdk::REMethodDefinition*> setters{};
        std::vector<Pair> pairs{};

        for (auto& m : t->get_methods()) {
            if (m.get_name() == nullptr || m.get_num_params() != 1 || m.is_static()) {
                continue;
            }

            const std::string n = m.get_name();

            if (n.starts_with("set_") || (n.size() > 3 && n.starts_with("set") && std::isupper((unsigned char)n[3]))) {
                setters[n] = &m;
            }
        }

        for (auto& m : t->get_methods()) {
            if (m.get_name() == nullptr || m.get_num_params() != 0 || m.is_static()) {
                continue;
            }

            const std::string n = m.get_name();
            std::string setter_name{};

            if (n.starts_with("get_")) {
                setter_name = "set_" + n.substr(4);
            } else if (n.size() > 3 && n.starts_with("get") && std::isupper((unsigned char)n[3])) {
                setter_name = "set" + n.substr(3);
            } else {
                continue;
            }

            if (const auto s = setters.find(setter_name); s != setters.end()) {
                pairs.emplace_back(&m, s->second);
            }
        }

        it = cache.emplace(t, std::move(pairs)).first;
    }

    size_t count = 0;

    for (const auto& [getter, setter] : it->second) {
        const auto* rt = getter->get_return_type();

        if (rt == nullptr || !rt->is_value_type()) {
            if (skipped != nullptr) {
                *skipped += (skipped->empty() ? "" : ", ") + std::string{getter->get_name()};
            }

            continue;
        }

        const bool by_ptr = rt->get_valuetype_size() > sizeof(void*) || (!rt->is_primitive() && !rt->is_enum());
        size_t size = rt->get_valuetype_size();

        if (size == 0 || size > 128) {
            size = by_ptr ? 128 : sizeof(void*);
        }

        const auto a = getter->invoke(src, std::span<void*>{});

        if (a.exception_thrown) {
            continue;
        }

        const auto b = getter->invoke(dst, std::span<void*>{});

        if (!b.exception_thrown && std::memcmp(a.bytes.data(), b.bytes.data(), size) == 0) {
            ++count;

            if (names != nullptr) {
                *names += (names->empty() ? "" : ", ") + std::string{getter->get_name()};
            }

            continue;
        }

        if (by_ptr) {
            setter->invoke(dst, (void*)a.bytes.data());
        } else {
            setter->invoke(dst, a.ptr);
        }

        ++count;

        if (names != nullptr) {
            *names += (names->empty() ? "" : ", ") + std::string{getter->get_name()};
        }
    }

    return count;
}

// [EXPOSURE TEST 01.11.2026] see ToneTest. The game camera's own values are kept and put back when the test ends.
void CameraDuplicator::apply_tone_test(void* ctx, REGameObject* main_gameobject, const std::vector<RECamera*>& clones, size_t wanted) {
    static const auto t = sdk::find_type_definition("via.render.ToneMapping");

    if (t == nullptr || main_gameobject == nullptr || main_gameobject->transform == nullptr) {
        return;
    }

    static const auto get_local = t->get_method("get_EnableLocalExposure");
    static const auto set_local = t->get_method("set_EnableLocalExposure");
    static const auto get_auto = t->get_method("getAutoExposure");
    static const auto set_auto = t->get_method("setAutoExposure");
    const auto* auto_type = get_auto != nullptr ? get_auto->get_return_type() : nullptr;
    // an enum / a number: set by value; anything else is copied through its bytes
    const bool auto_scalar = auto_type != nullptr && (auto_type->is_enum() || auto_type->is_primitive());
    const auto tt = m_tone_test;

    auto* main_comp = utility::re_component::find<REComponent>(main_gameobject->transform, "via.render.ToneMapping");

    if (main_comp == nullptr) {
        return;
    }

    // the game camera (all views)
    if (get_local != nullptr && set_local != nullptr) {
        if (tt.local_exposure == 2) {
            if (!m_main_local_saved) {
                m_main_local_value = get_local->call<bool>(ctx, main_comp);
                m_main_local_saved = true;
            }

            set_local->call(ctx, main_comp, false);
        } else if (m_main_local_saved) {
            set_local->call(ctx, main_comp, m_main_local_value);
            m_main_local_saved = false;
        }
    }

    if (get_auto != nullptr && set_auto != nullptr && auto_scalar) {
        if (tt.auto_value >= 0) {
            if (!m_main_auto_saved) {
                m_main_auto_value = get_auto->call<uint32_t>(ctx, main_comp);
                m_main_auto_saved = true;
            }

            set_auto->call(ctx, main_comp, (uint32_t)tt.auto_value);
        } else if (m_main_auto_saved) {
            set_auto->call(ctx, main_comp, m_main_auto_value);
            m_main_auto_saved = false;
        }
    }

    // [EV LOCK 13.11.2026] RE7 multipass dumps of 13.11.2026: the clone's eye up to 2x darker in the shadows than the game
    // camera's (the same scene), the fovea's views up to 4x darker or brighter than the wide view by what they see (sky
    // or not): each view's own automatic exposure. The component's values are all the same in every view (camera check);
    // the adapted exposure is computed on the GPU. Test: the range of the automatic exposure closed on the game camera's
    // SnappedRealEV, and the values in the framework log every 5 seconds (does SnappedRealEV follow the adaptation?).
    static const auto get_real = t->get_method("get_SnappedRealEV");
    static const auto get_ev = t->get_method("get_EV");
    static const auto get_use_auto = t->get_method("get_UseAutoExposure");
    static const auto get_min = t->get_method("get_AutoExposureMinEV");
    static const auto get_max = t->get_method("get_AutoExposureMaxEV");
    static const auto set_min = t->get_method("set_AutoExposureMinEV");
    static const auto set_max = t->get_method("set_AutoExposureMaxEV");
    static const auto set_ev = t->get_method("set_EV");
    static const auto set_bright_rate = t->get_method("set_BrightAdaptationRate");
    static const auto set_dark_rate = t->get_method("set_DarkAdaptationRate");
    static const auto get_bright_rate = t->get_method("get_BrightAdaptationRate");
    static const auto get_dark_rate = t->get_method("get_DarkAdaptationRate");
    const bool can_lock = get_real != nullptr && get_min != nullptr && get_max != nullptr && set_min != nullptr && set_max != nullptr;
    const float main_ev = get_ev != nullptr ? get_ev->call<float>(ctx, main_comp) : 0.0f;

    // [EV FOLLOW 13.11.2026] the effect test of 13.11.2026 (RE7, facing the house and the sun) found no engine effect behind
    // the fovea / periphery difference (19%, as on RE4), and in the plain multipass the clone's eye, the same view as the
    // game camera's, is up to 2x darker in the shadows with the sky not blown out: the look of a lower exposure. RE7's
    // ToneMapping has no setter for the automatic exposure, but an EV setter: the fovea views get the game camera's EV
    // plus an offset, fixed (menu) and / or following the measured brightness ratio of periphery and fovea.
    if (tt.ev_follow) {
        auto& qv = quadview::State::get();

        for (int s = 0; s < 2; ++s) {
            const uint64_t n = qv.view_gain_frames[s].load(std::memory_order_relaxed);

            if (n == m_ev_follow_seen[s]) {
                continue;
            }

            m_ev_follow_seen[s] = n;
            const float g = qv.view_gain[s].load(std::memory_order_relaxed);

            if (!(g > 0.01f && g < 100.0f)) {
                continue;
            }

            // [EV FOLLOW 13.11.2026 evening] test of 18:57: the direction check flipped the direction at every look from the
            // sky to the trees (the error grows while the view changes, not because of the offset): with the wrong direction
            // the fovea went almost black for a second. A lower EV brightens (the right eye with -0.16: brighter; the
            // follow converged to gain 1 only in that direction): the direction is fixed. The error is smoothed (the
            // measurement comes some frames late and the engine's own exposure keeps adapting) and steps are smaller.
            const float e = std::log2(g); // > 0: the fovea darker than the periphery
            // [EV FOLLOW 13.11.2026 19:55] test: too slow (about 0.7 EV per second): twice the step, a little less smoothing
            m_ev_follow_error[s] += 0.35f * (std::clamp(e, -2.0f, 2.0f) - m_ev_follow_error[s]);
            const float es = m_ev_follow_error[s];

            if (std::abs(es) > 0.02f) {
                m_ev_follow_offset[s] = std::clamp(m_ev_follow_offset[s] - 0.06f * std::clamp(es, -1.0f, 1.0f), -4.0f, 4.0f);
            }
        }
    } else {
        m_ev_follow_offset[0] = m_ev_follow_offset[1] = 0.0f;
        m_ev_follow_error[0] = m_ev_follow_error[1] = 0.0f;
    }
    // [REAL EV 13.11.2026] RE7 test of 13.11.2026 21:09 (fix44, calcRealEV in the 5 s log): after calcRealEV the game camera's
    // SnappedRealEV is its adapted exposure (6.8 .. 9.4 while its EV is 3.5 .. 4.5; without the call it stays equal to EV, which
    // is why the range lock on it changed nothing). The fovea views, their adaptation stopped since the main menu (rates 0),
    // showed SnappedRealEV = EV, and the EV follow brought their EV to the game camera's SnappedRealEV within 0.07 by the
    // measured brightness alone (21:11:30 8.402 / 8.406, 21:11:50 6.841 / 6.820, 21:12:20 7.982 / 7.906). Called while this
    // mode or the range lock is on; its CPU cost in the 5 s log.
    // [REAL EV 13.11.2026 21:52] test of fix45: the call costs 0.73 ms of the main thread (up to 9.6 ms): every 4th frame (the
    // adaptation moves slowly), SnappedRealEV keeps the last value in between.
    // [REAL EV fix105 20.11.2026] RE4 logs of 20.11.2026 (three sessions): the call costs 0.69 - 0.70 ms of the main thread, and
    // every 4th frame it was made three times, on the game camera and on each fovea view (LockScene 0.71 ms on average with
    // peaks of 2.5: in the part of the main thread's frame the render thread waits for). The fovea views' own offset, which
    // their two calls measure, was 0.0000 in all 1208 logged values (their adaptation is stopped), and the game camera's
    // value is the same 5 s later in 29 - 64 % of the logged pairs of the three sessions (at the end of its range, or its
    // automatic exposure off). Now:
    //  - a fovea view: every 4th frame until its own offset has been 0 in 8 measurements in a row, then every 128th, in a
    //    frame without another call (a value other than 0, another component or another mode puts it back). While the
    //    offset is 0 the EV set is the same as with the measurement: same image;
    //  - the game camera: every 4th frame while its value moves, as before; when a call gives the value of the call before
    //    (within 0.002 EV) the next one comes after 8 frames (ToneTest::real_ev_steady; off: every 4th always). A change
    //    puts it back to every 4th. What it costs: when the exposure starts to move after a steady stretch, the fovea
    //    views learn it up to 7 frames late instead of 3 (with the rates logged, 0.5 / 0.8, some hundredths of an EV);
    //  - a change of the game camera's exposure mode (its ToneMapping component, UseAutoExposure or EV: the game's menus
    //    and cutscenes change them, 3.5 with the automatic exposure against 4.0 without) is seen with cheap getters every
    //    frame and measured at once, though never before the 4th frame after the last call (as before).
    static const auto calc_real = t->get_method("calcRealEV");
    const bool real_wanted = calc_real != nullptr && get_real != nullptr && (tt.real_ev != 0 || tt.ev_lock != 0);
    const uint32_t real_frame = real_wanted ? m_real_ev_frame++ : 0u;
    const bool clone_tick = real_wanted && (real_frame % 4) == 0; // the fovea views' measurements keep the old frames
    bool main_calc = false;
    bool mode_changed = false;

    if (real_wanted) {
        const bool main_auto = get_use_auto != nullptr ? get_use_auto->call<bool>(ctx, main_comp) : false;
        mode_changed = (void*)main_comp != m_real_ev_comp || main_auto != m_real_ev_auto || std::abs(main_ev - m_real_ev_ev) > 0.0005f;
        m_real_ev_comp = (void*)main_comp;
        m_real_ev_auto = main_auto;
        m_real_ev_ev = main_ev;
        m_real_ev_since = (std::min)(m_real_ev_since + 1u, 1000u);
        m_real_ev_mode_pending |= mode_changed;
        main_calc = m_real_ev_since >= m_real_ev_interval || (m_real_ev_mode_pending && m_real_ev_since >= 4);
    } else {
        m_real_ev_interval = 4;
        m_real_ev_since = 1000;
        m_real_ev_mode_pending = false;
        m_real_ev_comp = nullptr;
    }

    if (main_calc) {
#if REVR_RELEASE // [RELEASE SWITCH fix141] the call's timing is for the log only
        calc_real->call(ctx, main_comp);
#else
        const auto t0 = std::chrono::high_resolution_clock::now();
        calc_real->call(ctx, main_comp);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
        m_real_ev_calc_us_sum += us;
        m_real_ev_calc_us_max = (std::max)(m_real_ev_calc_us_max, us);
        ++m_real_ev_calc_frames;

        if (m_real_ev_mode_pending && m_real_ev_since < m_real_ev_interval) {
            ++m_real_ev_mode_calls;
        }
#endif

        const float now = get_real->call<float>(ctx, main_comp);
        const bool steady = tt.real_ev_steady && !m_real_ev_mode_pending && std::abs(now - m_real_ev_prev) < 0.002f;
        m_real_ev_interval = steady ? 8u : 4u;
        m_real_ev_prev = now;
        m_real_ev_since = 0;
        m_real_ev_mode_pending = false;
    }

    const float main_real = get_real != nullptr ? get_real->call<float>(ctx, main_comp) : 0.0f;

    if (can_lock && tt.ev_lock == 2) {
        if (!m_main_range_saved) {
            m_main_range_saved_min = get_min->call<float>(ctx, main_comp);
            m_main_range_saved_max = get_max->call<float>(ctx, main_comp);
            m_main_range_saved = true;
            REVR_LOG(spdlog::info("[CameraDuplicator] exposure lock: game camera range {}..{} closed on {}", m_main_range_saved_min, m_main_range_saved_max,
                main_real));
        }

        set_min->call(ctx, main_comp, main_real);
        set_max->call(ctx, main_comp, main_real);
    } else if (m_main_range_saved) {
        set_min->call(ctx, main_comp, m_main_range_saved_min);
        set_max->call(ctx, main_comp, m_main_range_saved_max);
        m_main_range_saved = false;
        REVR_LOG(spdlog::info("[CameraDuplicator] exposure lock: game camera range {}..{} restored", m_main_range_saved_min, m_main_range_saved_max));
    }

    // [WHITE POINT 13.11.2026] calcRealEV(): does it bring the adapted exposure into SnappedRealEV? (logged as "after calc")
    const auto exposure_text = [&](REComponent* c) {
        // ([REAL EV fix105] the log no longer calls calcRealEV itself: three calls of 0.7 ms every 5 s on the main thread; the
        // values are the ones the last scheduled call left)
        const float real_before = get_real != nullptr ? get_real->call<float>(ctx, c) : -99.0f;

        return std::format("real as last computed {:.3f}, adaptation {:.3f}/{:.3f} EV {:.3f} real {:.3f} auto {} range {:.2f}..{:.2f}", real_before,
            get_bright_rate != nullptr ? get_bright_rate->call<float>(ctx, c) : -99.0f, get_dark_rate != nullptr ? get_dark_rate->call<float>(ctx, c) : -99.0f,
            get_ev != nullptr ? get_ev->call<float>(ctx, c) : -99.0f,
            get_real != nullptr ? get_real->call<float>(ctx, c) : -99.0f, get_use_auto != nullptr ? get_use_auto->call<bool>(ctx, c) : false,
            get_min != nullptr ? get_min->call<float>(ctx, c) : -99.0f, get_max != nullptr ? get_max->call<float>(ctx, c) : -99.0f);
    };

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    std::string exposure_log{};
    const bool log_now = ++m_exposure_log_frames % 450 == 0;

    if (log_now) {
        exposure_log = std::format("game camera {}", exposure_text(main_comp));
    }
#endif

    // the clones (the fovea views)
    bool settled_measured = false; // [REAL EV fix105] a settled view's check made in this frame: the other waits for the next

    for (size_t i = 0; i < clones.size() && i < wanted; ++i) {
        const auto go = clones[i] != nullptr ? utility::re_component::get_game_object((REComponent*)clones[i]) : nullptr;
        auto* comp = go != nullptr && go->transform != nullptr ? utility::re_component::find<REComponent>(go->transform, "via.render.ToneMapping") : nullptr;

        if (comp == nullptr) {
            continue;
        }

        // (after the copies of this frame, which bring the game camera's range back every frame)
        if (can_lock && tt.ev_lock >= 1) {
            set_min->call(ctx, comp, main_real);
            set_max->call(ctx, comp, main_real);
        }

        // [ADAPT 13.11.2026] the fovea views' own adaptation (after the copies, which bring the game camera's rates)
        if (tt.clone_adapt != 0 && set_bright_rate != nullptr && set_dark_rate != nullptr) {
            const float rate = tt.clone_adapt == 1 ? 0.0f : 1000.0f;
            set_bright_rate->call(ctx, comp, rate);
            set_dark_rate->call(ctx, comp, rate);
        }

        // [REAL EV 13.11.2026] their own adaptation stopped (mode 1)
        if (tt.real_ev == 1 && set_bright_rate != nullptr && set_dark_rate != nullptr) {
            set_bright_rate->call(ctx, comp, 0.0f);
            set_dark_rate->call(ctx, comp, 0.0f);
        }

        // [REAL EV 13.11.2026] the game camera's adapted exposure as their EV, their own remaining offset taken off
        if (tt.real_ev != 0 && real_wanted && set_ev != nullptr && get_ev != nullptr && i < MAX_CLONES) {
            // the copies of the game camera's properties may have put its EV back this frame: their own offset is measured
            // only while the EV is still the one set the frame before
            const float ev_now = get_ev->call<float>(ctx, comp);

            // [REAL EV fix105] another component or another mode: measured every 4th frame again
            if ((void*)comp != m_real_ev_clone_comp[i] || tt.real_ev != m_real_ev_mode_seen[i]) {
                m_real_ev_clone_comp[i] = (void*)comp;
                m_real_ev_mode_seen[i] = tt.real_ev;
                m_real_ev_own_zero[i] = 0;
            }

            // ([REAL EV fix105] settled at 0: measured every 128th frame only, in a frame without another calcRealEV)
            bool measure = false;

            if (m_real_ev_own_zero[i] < 8) {
                measure = clone_tick;
            } else if (m_real_ev_clone_wait[i] > 0) {
                --m_real_ev_clone_wait[i];
            } else {
                measure = !main_calc && !settled_measured;
            }

            if (measure && std::abs(ev_now - m_real_ev_last_set[i]) < 0.25f) {
                const bool was_settled = m_real_ev_own_zero[i] >= 8;
#if REVR_RELEASE // [RELEASE SWITCH fix141] the call's timing is for the log only
                calc_real->call(ctx, comp);
#else
                const auto t0 = std::chrono::high_resolution_clock::now();
                calc_real->call(ctx, comp);
                m_real_ev_clone_us_sum += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
                ++m_real_ev_clone_calls;
#endif
                const float own = get_real->call<float>(ctx, comp) - ev_now;

                if (std::isfinite(own) && std::abs(own) < 8.0f) {
                    m_real_ev_clone_offset[i] += 0.2f * (own - m_real_ev_clone_offset[i]);
                }

                const bool zero = std::abs(own) < 0.0005f && std::abs(m_real_ev_clone_offset[i]) < 0.0005f;
                m_real_ev_own_zero[i] = zero ? (uint8_t)(std::min)(m_real_ev_own_zero[i] + 1, 8) : (uint8_t)0;

                if (m_real_ev_own_zero[i] >= 8) {
                    // the views' checks spread over the frames (the first after 64 frames for one view, 128 for the other)
                    m_real_ev_clone_wait[i] = was_settled ? 128u : 64u + 64u * (uint32_t)(i % 2);
                    settled_measured |= was_settled;
                }
            }

            const float off = tt.ev_offset + (tt.ev_follow && i < 2 ? m_ev_follow_offset[i] : 0.0f);
            const float target = main_real + off - m_real_ev_clone_offset[i];
            set_ev->call(ctx, comp, target);
            m_real_ev_last_set[i] = target;
        } else if (set_ev != nullptr && get_ev != nullptr && (tt.ev_offset != 0.0f || tt.ev_follow)) { // [EV FOLLOW 13.11.2026]
            const float off = tt.ev_offset + (tt.ev_follow && i < 2 ? m_ev_follow_offset[i] : 0.0f);
            set_ev->call(ctx, comp, main_ev + off);
        }

        if (tt.real_ev == 0 && i < MAX_CLONES) {
            m_real_ev_clone_offset[i] = 0.0f;
            m_real_ev_last_set[i] = -1000.0f;
            m_real_ev_own_zero[i] = 0; // [REAL EV fix105]
        }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        if (log_now) {
            exposure_log += std::format(" | clone {} {}", i, exposure_text(comp));

            if (tt.ev_follow && i < 2) {
                exposure_log += std::format(" follow {:+.2f} (gain {:.3f}, smoothed error {:+.3f})", m_ev_follow_offset[i],
                    quadview::State::get().view_gain[i].load(std::memory_order_relaxed), m_ev_follow_error[i]);
            } else if (tt.real_ev != 0 && i < 2) {
                exposure_log += std::format(" own offset {:+.3f} (gain {:.3f})", m_real_ev_clone_offset[i],
                    quadview::State::get().view_gain[i].load(std::memory_order_relaxed));
            }
        }
#endif

        if (tt.local_exposure >= 1 && set_local != nullptr) {
            set_local->call(ctx, comp, false);
        }

        if (get_auto == nullptr || set_auto == nullptr) {
            continue;
        }

        // [REAL EV 13.11.2026] modes 2 / 3: the fovea views' automatic exposure Disable / FixedEnable
        const int clone_auto = tt.real_ev == 2 ? 2 : tt.real_ev == 3 ? 1 : tt.auto_value;
        const bool changes = (clone_auto >= 0 || tt.copy_auto) && auto_scalar;

        if (changes && !m_clone_auto_saved.contains(clones[i])) {
            m_clone_auto_saved[clones[i]] = get_auto->call<uint32_t>(ctx, comp);
        } else if (!changes && auto_scalar && m_clone_auto_saved.contains(clones[i])) {
            set_auto->call(ctx, comp, m_clone_auto_saved[clones[i]]);
            m_clone_auto_saved.erase(clones[i]);
        }

        if (clone_auto >= 0 && auto_scalar) {
            set_auto->call(ctx, comp, (uint32_t)clone_auto);
        } else if (tt.copy_auto) {
            if (auto_scalar) {
                const auto v = get_auto->call<uint32_t>(ctx, main_comp);

                if (get_auto->call<uint32_t>(ctx, comp) != v) {
                    set_auto->call(ctx, comp, v);
                }
            } else {
                auto r = get_auto->invoke(main_comp, std::span<void*>{});

                if (!r.exception_thrown) {
                    set_auto->invoke(comp, (void*)r.bytes.data());
                }
            }
        }
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    if (log_now) {
        // [REAL EV 13.11.2026] the game camera's calcRealEV each frame: its CPU cost
        std::string cost{};

        if (m_real_ev_calc_frames > 0) {
            // [REAL EV fix105] how often it is called now (of 450 frames), and the fovea views' calls
            cost = std::format(", calcRealEV on the game camera {:.1f} us per call, {} calls in 450 frames ({} of them early, after a change of its "
                               "exposure mode), now every {}th (max {:.1f}); on the fovea views {} calls, {:.1f} us each",
                m_real_ev_calc_us_sum / m_real_ev_calc_frames, m_real_ev_calc_frames, m_real_ev_mode_calls, m_real_ev_interval, m_real_ev_calc_us_max,
                m_real_ev_clone_calls, m_real_ev_clone_calls > 0 ? m_real_ev_clone_us_sum / m_real_ev_clone_calls : 0.0);
            m_real_ev_mode_calls = 0;
            m_real_ev_calc_us_sum = m_real_ev_calc_us_max = 0.0;
            m_real_ev_calc_frames = 0;
            m_real_ev_clone_calls = 0;
            m_real_ev_clone_us_sum = 0.0;
        }

        REVR_LOG(spdlog::info("[CameraDuplicator] exposure (lock {}, EV offset {:+.2f}, follow {}, real EV mode {}{}): {}", tt.ev_lock, tt.ev_offset, tt.ev_follow,
            tt.real_ev, cost, exposure_log));
    }
#endif
}

sdk::SystemArray* CameraDuplicator::get_all_cameras() {
    const auto scene = sdk::get_current_scene();

    if (scene == nullptr) {
        return nullptr;
    }

    const auto camera_typeof = sdk::find_type_definition("via.Camera")->get_runtime_type();
    const auto existing_cameras = sdk::call_object_func_easy<sdk::SystemArray*>(scene, "findComponents(System.Type)", camera_typeof);

    if (existing_cameras == nullptr) {
        return nullptr;
    }

    return existing_cameras;
}
}
