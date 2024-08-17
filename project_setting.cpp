#include "project_settings.h"

#include "core/core_bind.h" // For Compression enum.
#include "core/input/input_map.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/file_access_pack.h"
#include "core/io/marshalls.h"
#include "core/io/resource_uid.h"
#include "core/object/script_language.h"
#include "core/os/keyboard.h"
#include "core/templates/rb_set.h"
#include "core/variant/typed_array.h"
#include "core/variant/variant_parser.h"
#include "core/version.h"

#ifdef TOOLS_ENABLED
#include "modules/modules_enabled.gen.h" // For mono.
#endif // TOOLS_ENABLED

const String ProjectSettings::PROJECT_DATA_DIR_NAME_SUFFIX = "godot";

ProjectSettings *ProjectSettings::singleton = nullptr;

ProjectSettings *ProjectSettings::get_singleton() {
	return singleton;
}

String ProjectSettings::get_project_data_dir_name() const {
	return project_data_dir_name;
}

String ProjectSettings::get_project_data_path() const {
	return "res://" + get_project_data_dir_name();
}

String ProjectSettings::get_resource_path() const {
	return resource_path;
}

String ProjectSettings::get_imported_files_path() const {
	return get_project_data_path().path_join("imported");
}

#ifdef TOOLS_ENABLED
// Returns the features that a project must have when opened with this build of Godot.
// This is used by the project manager to provide the initial_settings for config/features.
const PackedStringArray ProjectSettings::get_required_features() {
	PackedStringArray features;
	features.append(VERSION_BRANCH);
#ifdef REAL_T_IS_DOUBLE
	features.append("Double Precision");
#endif
	return features;
}

// Returns the features supported by this build of Godot. Includes all required features.
const PackedStringArray ProjectSettings::_get_supported_features() {
	PackedStringArray features = get_required_features();
#ifdef MODULE_MONO_ENABLED
	features.append("C#");
#endif
	// Allow pinning to a specific patch number or build type by marking
	// them as supported. They're only used if the user adds them manually.
	features.append(VERSION_BRANCH "." _MKSTR(VERSION_PATCH));
	features.append(VERSION_FULL_CONFIG);
	features.append(VERSION_FULL_BUILD);

#ifdef RD_ENABLED
	features.append("Forward Plus");
	features.append("Mobile");
#endif

#ifdef GLES3_ENABLED
	features.append("GL Compatibility");
#endif
	return features;
}

// Returns the features that this project needs but this build of Godot lacks.
const PackedStringArray ProjectSettings::get_unsupported_features(const PackedStringArray &p_project_features) {
	PackedStringArray unsupported_features;
	PackedStringArray supported_features = singleton->_get_supported_features();
	for (int i = 0; i < p_project_features.size(); i++) {
		if (!supported_features.has(p_project_features[i])) {
			// Temporary compatibility code to ease upgrade to 4.0 beta 2+.
			if (p_project_features[i].begins_with("Vulkan")) {
				continue;
			}
			unsupported_features.append(p_project_features[i]);
		}
	}
	unsupported_features.sort();
	return unsupported_features;
}

// Returns the features that both this project has and this build of Godot has, ensuring required features exist.
const PackedStringArray ProjectSettings::_trim_to_supported_features(const PackedStringArray &p_project_features) {
	// Remove unsupported features if present.
	PackedStringArray features = PackedStringArray(p_project_features);
	PackedStringArray supported_features = _get_supported_features();
	for (int i = p_project_features.size() - 1; i > -1; i--) {
		if (!supported_features.has(p_project_features[i])) {
			features.remove_at(i);
		}
	}
	// Add required features if not present.
	PackedStringArray required_features = get_required_features();
	for (int i = 0; i < required_features.size(); i++) {
		if (!features.has(required_features[i])) {
			features.append(required_features[i]);
		}
	}
	features.sort();
	return features;
}
#endif // TOOLS_ENABLED

String ProjectSettings::localize_path(const String &p_path) const {
	String path = p_path.simplify_path();

	if (resource_path.is_empty() || (path.is_absolute_path() && !path.begins_with(resource_path))) {
		return path;
	}

	// Check if we have a special path (like res://) or a protocol identifier.
	int p = path.find("://");
	bool found = false;
	if (p > 0) {
		found = true;
		for (int i = 0; i < p; i++) {
			if (!is_ascii_alphanumeric_char(path[i])) {
				found = false;
				break;
			}
		}
	}
	if (found) {
		return path;
	}

	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);

	if (dir->change_dir(path) == OK) {
		String cwd = dir->get_current_dir();
		cwd = cwd.replace("\\", "/");

		// Ensure that we end with a '/'.
		// This is important to ensure that we do not wrongly localize the resource path
		// in an absolute path that just happens to contain this string but points to a
		// different folder (e.g. "/my/project" as resource_path would be contained in
		// "/my/project_data", even though the latter is not part of res://.
		// `path_join("")` is an easy way to ensure we have a trailing '/'.
		const String res_path = resource_path.path_join("");

		// DirAccess::get_current_dir() is not guaranteed to return a path that with a trailing '/',
		// so we must make sure we have it as well in order to compare with 'res_path'.
		cwd = cwd.path_join("");

		if (!cwd.begins_with(res_path)) {
			return path;
		}

		return cwd.replace_first(res_path, "res://");
	} else {
		int sep = path.rfind("/");
		if (sep == -1) {
			return "res://" + path;
		}

		String parent = path.substr(0, sep);

		String plocal = localize_path(parent);
		if (plocal.is_empty()) {
			return "";
		}
		// Only strip the starting '/' from 'path' if its parent ('plocal') ends with '/'
		if (plocal[plocal.length() - 1] == '/') {
			sep += 1;
		}
		return plocal + path.substr(sep, path.size() - sep);
	}
}

void ProjectSettings::set_initial_value(const String &p_name, const Variant &p_value) {
	ERR_FAIL_COND_MSG(!props.has(p_name), "Request for nonexistent project setting: " + p_name + ".");

	// Duplicate so that if value is array or dictionary, changing the setting will not change the stored initial value.
	props[p_name].initial = p_value.duplicate();
}

void ProjectSettings::set_restart_if_changed(const String &p_name, bool p_restart) {
	ERR_FAIL_COND_MSG(!props.has(p_name), "Request for nonexistent project setting: " + p_name + ".");
	props[p_name].restart_if_changed = p_restart;
}

void ProjectSettings::set_as_basic(const String &p_name, bool p_basic) {
	ERR_FAIL_COND_MSG(!props.has(p_name), "Request for nonexistent project setting: " + p_name + ".");
	props[p_name].basic = p_basic;
}

void ProjectSettings::set_as_internal(const String &p_name, bool p_internal) {
	ERR_FAIL_COND_MSG(!props.has(p_name), "Request for nonexistent project setting: " + p_name + ".");
	props[p_name].internal = p_internal;
}

void ProjectSettings::set_ignore_value_in_docs(const String &p_name, bool p_ignore) {
	ERR_FAIL_COND_MSG(!props.has(p_name), "Request for nonexistent project setting: " + p_name + ".");
#ifdef DEBUG_METHODS_ENABLED
	props[p_name].ignore_value_in_docs = p_ignore;
#endif
}

bool ProjectSettings::get_ignore_value_in_docs(const String &p_name) const {
	ERR_FAIL_COND_V_MSG(!props.has(p_name), false, "Request for nonexistent project setting: " + p_name + ".");
#ifdef DEBUG_METHODS_ENABLED
	return props[p_name].ignore_value_in_docs;
#else
	return false;
#endif
}

void ProjectSettings::add_hidden_prefix(const String &p_prefix) {
	ERR_FAIL_COND_MSG(hidden_prefixes.has(p_prefix), vformat("Hidden prefix '%s' already exists.", p_prefix));
	hidden_prefixes.push_back(p_prefix);
}

String ProjectSettings::globalize_path(const String &p_path) const {
	if (p_path.begins_with("res://")) {
		if (!resource_path.is_empty()) {
			return p_path.replace("res:/", resource_path);
		}
		return p_path.replace("res://", "");
	} else if (p_path.begins_with("user://")) {
		String data_dir = OS::get_singleton()->get_user_data_dir();
		if (!data_dir.is_empty()) {
			return p_path.replace("user:/", data_dir);
		}
		return p_path.replace("user://", "");
	}

	return p_path;
}

bool ProjectSettings::_set(const StringName &p_name, const Variant &p_value) {
	_THREAD_SAFE_METHOD_

	if (p_value.get_type() == Variant::NIL) {
		props.erase(p_name);
		if (p_name.operator String().begins_with("autoload/")) {
			String node_name = p_name.operator String().split("/")[1];
			if (autoloads.has(node_name)) {
				remove_autoload(node_name);
			}
		} else if (p_name.operator String().begins_with("global_group/")) {
			String group_name = p_name.operator String().get_slice("/", 1);
			if (global_groups.has(group_name)) {
				remove_global_group(group_name);
			}
		}
	} else {
		if (p_name == CoreStringName(_custom_features)) {
			Vector<String> custom_feature_array = String(p_value).split(",");
			for (int i = 0; i < custom_feature_array.size(); i++) {
				custom_features.insert(custom_feature_array[i]);
			}
			_queue_changed();
			return true;
		}

		{ // Feature overrides.
			int dot = p_name.operator String().find(".");
			if (dot != -1) {
				Vector<String> s = p_name.operator String().split(".");

				for (int i = 1; i < s.size(); i++) {
					String feature = s[i].strip_edges();
					Pair<StringName, StringName> feature_override(feature, p_name);

					if (!feature_overrides.has(s[0])) {
						feature_overrides[s[0]] = LocalVector<Pair<StringName, StringName>>();
					}

					feature_overrides[s[0]].push_back(feature_override);
				}
			}
		}

		if (props.has(p_name)) {
			props[p_name].variant = p_value;
		} else {
			props[p_name] = VariantContainer(p_value, last_order++);
		}
		if (p_name.operator String().begins_with("autoload/")) {
			String node_name = p_name.operator String().split("/")[1];
			AutoloadInfo autoload;
			autoload.name = node_name;
			String path = p_value;
			if (path.begins_with("*")) {
				autoload.is_singleton = true;
				autoload.path = path.substr(1).simplify_path();
			} else {
				autoload.path = path.simplify_path();
			}
			add_autoload(autoload);
		} else if (p_name.operator String().begins_with("global_group/")) {
			String group_name = p_name.operator String().get_slice("/", 1);
			add_global_group(group_name, p_value);
		}
	}

	_queue_changed();
	return true;
}

bool ProjectSettings::_get(const StringName &p_name, Variant &r_ret) const {
	_THREAD_SAFE_METHOD_

	if (!props.has(p_name)) {
		WARN_PRINT("Property not found: " + String(p_name));
		return false;
	}
	r_ret = props[p_name].variant;
	return true;
}

Variant ProjectSettings::get_setting_with_override(const StringName &p_name) const {
	_THREAD_SAFE_METHOD_

	StringName name = p_name;
	if (feature_overrides.has(name)) {
		const LocalVector<Pair<StringName, StringName>> &overrides = feature_overrides[name];
		for (uint32_t i = 0; i < overrides.size(); i++) {
			if (OS::get_singleton()->has_feature(overrides[i].first)) { // Custom features are checked in OS.has_feature() already. No need to check twice.
				if (props.has(overrides[i].second)) {
					name = overrides[i].second;
					break;
				}
			}
		}
	}

	if (!props.has(name)) {
		WARN_PRINT("Property not found: " + String(name));
		return Variant();
	}
	return props[name].variant;
}

struct _VCSort {
	String name;
	Variant::Type type = Variant::VARIANT_MAX;
	int order = 0;
	uint32_t flags = 0;

	bool operator<(const _VCSort &p_vcs) const { return order == p_vcs.order ? name < p_vcs.name : order < p_vcs.order; }
};

void ProjectSettings::_get_property_list(List<PropertyInfo> *p_list) const {
	_THREAD_SAFE_METHOD_

	RBSet<_VCSort> vclist;

	for (const KeyValue<StringName, VariantContainer> &E : props) {
		const VariantContainer *v = &E.value;

		if (v->hide_from_editor) {
			continue;
		}

		_VCSort vc;
		vc.name = E.key;
		vc.order = v->order;
		vc.type = v->variant.get_type();

		bool internal = v->internal;
		if (!internal) {
			for (const String &F : hidden_prefixes) {
				if (vc.name.begins_with(F)) {
					internal = true;
					break;
				}
			}
		}

		if (internal) {
			vc.flags = PROPERTY_USAGE_STORAGE;
		} else {
			vc.flags = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE;
		}

		if (v->internal) {
			vc.flags |= PROPERTY_USAGE_INTERNAL;
		}

		if (v->basic) {
			vc.flags |= PROPERTY_USAGE_EDITOR_BASIC_SETTING;
		}

		if (v->restart_if_changed) {
			vc.flags |= PROPERTY_USAGE_RESTART_IF_CHANGED;
		}
		vclist.insert(vc);
	}

	for (const _VCSort &E : vclist) {
		String prop_info_name = E.name;
		int dot = prop_info_name.find(".");
		if (dot != -1 && !custom_prop_info.has(prop_info_name)) {
			prop_info_name = prop_info_name.substr(0, dot);
		}

		if (custom_prop_info.has(prop_info_name)) {
			PropertyInfo pi = custom_prop_info[prop_info_name];
			pi.name = E.name;
			pi.usage = E.flags;
			p_list->push_back(pi);
		} else {
			p_list->push_back(PropertyInfo(E.type, E.name, PROPERTY_HINT_NONE, "", E.flags));
		}
	}
}

void ProjectSettings::_queue_changed() {
	if (is_changed || !MessageQueue::get_singleton() || MessageQueue::get_singleton()->get_max_buffer_usage() == 0) {
		return;
	}
	is_changed = true;
	callable_mp(this, &ProjectSettings::_emit_changed).call_deferred();
}

void ProjectSettings::_emit_changed() {
	if (!is_changed) {
		return;
	}
	is_changed = false;
	emit_signal("settings_changed");
}

bool ProjectSettings::_load_resource_pack(const String &p_pack, bool p_replace_files, int p_offset) {
	if (PackedData::get_singleton()->is_disabled()) {
		return false;
	}

	bool ok = PackedData::get_singleton()->add_pack(p_pack, p_replace_files, p_offset) == OK;

	if (!ok) {
		return false;
	}

	if (project_loaded) {
		// This pack may have declared new global classes (make sure they are picked up).
		refresh_global_class_list();

		// This pack may have defined new UIDs, make sure they are cached.
		ResourceUID::get_singleton()->load_from_cache(false);
	}

	//if data.pck is found, all directory access will be from here
	DirAccess::make_default<DirAccessPack>(DirAccess::ACCESS_RESOURCES);
	using_datapack = true;

	return true;
}

void ProjectSettings::_convert_to_last_version(int p_from_version) {
	if (p_from_version <= 3) {
		// Converts the actions from array to dictionary (array of events to dictionary with deadzone + events)
		for (KeyValue<StringName, ProjectSettings::VariantContainer> &E : props) {
			Variant value = E.value.variant;
			if (String(E.key).begins_with("input/") && value.get_type() == Variant::ARRAY) {
				Array array = value;
				Dictionary action;
				action["deadzone"] = Variant(0.5f);
				action["events"] = array;
				E.value.variant = action;
			}
Error ProjectSettings::_setup(const String &p_path, const String &p_main_pack, bool p_upwards, bool p_ignore_override) {
	if (!OS::get_singleton()->get_resource_dir().is_empty()) {
		// OS will call ProjectSettings->get_resource_path which will be empty if not overridden!
		// If the OS would rather use a specific location, then it will not be empty.
		resource_path = OS::get_singleton()->get_resource_dir().replace("\\", "/");
		if (!resource_path.is_empty() && resource_path[resource_path.length() - 1] == '/') {
			resource_path = resource_path.substr(0, resource_path.length() - 1); // Chop end.
		}
	}

	// Attempt with a user-defined main pack first

	if (!p_main_pack.is_empty()) {
		bool ok = _load_resource_pack(p_main_pack);
		ERR_FAIL_COND_V_MSG(!ok, ERR_CANT_OPEN, "Cannot open resource pack '" + p_main_pack + "'.");

		Error err = _load_settings_text_or_binary("res://project.godot", "res://project.binary");
		if (err == OK && !p_ignore_override) {
			// Load override from location of the main pack
			// Optional, we don't mind if it fails
			_load_settings_text(p_main_pack.get_base_dir().path_join("override.cfg"));
		}
		return err;
	}

	String exec_path = OS::get_singleton()->get_executable_path();

	if (!exec_path.is_empty()) {
		// We do several tests sequentially until one succeeds to find a PCK,
		// and if so, we attempt loading it at the end.

		// Attempt with PCK bundled into executable.
		bool found = _load_resource_pack(exec_path);

		// Attempt with exec_name.pck.
		// (This is the usual case when distributing a Godot game.)
		String exec_dir = exec_path.get_base_dir();
		String exec_filename = exec_path.get_file();
		String exec_basename = exec_filename.get_basename();
