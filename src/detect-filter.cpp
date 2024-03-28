#include "detect-filter.h"

#include <onnxruntime_cxx_api.h>

#ifdef _WIN32
#include <wchar.h>
#include <windows.h>
#endif // _WIN32

#include <opencv2/imgproc.hpp>

#include <numeric>
#include <memory>
#include <exception>
#include <fstream>
#include <new>
#include <mutex>
#include <regex>
#include <thread>

#include <plugin-support.h>
#include "FilterData.h"
#include "consts.h"
#include "obs-utils/obs-utils.h"
#include "edgeyolo/utils.hpp"

struct detect_filter : public filter_data {};

const char *detect_filter_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Detect");
}

/**                   PROPERTIES                     */

static bool visible_on_bool(obs_properties_t *ppts, obs_data_t *settings,
			    const char *bool_prop, const char *prop_name)
{
	const bool enabled = obs_data_get_bool(settings, bool_prop);
	obs_property_t *p = obs_properties_get(ppts, prop_name);
	obs_property_set_visible(p, enabled);
	return true;
}

static bool enable_advanced_settings(obs_properties_t *ppts, obs_property_t *p,
				     obs_data_t *settings)
{
	const bool enabled = obs_data_get_bool(settings, "advanced");

	for (const char *prop_name :
	     {"threshold", "useGPU", "preview", "numThreads", "object_category",
	      "masking_group", "tracking_group"}) {
		p = obs_properties_get(ppts, prop_name);
		obs_property_set_visible(p, enabled);
	}

	return true;
}

obs_properties_t *detect_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *advanced = obs_properties_add_bool(
		props, "advanced", obs_module_text("Advanced"));

	// If advanced is selected show the advanced settings, otherwise hide them
	obs_property_set_modified_callback(advanced, enable_advanced_settings);

	obs_properties_add_float_slider(props, "threshold",
					obs_module_text("ConfThreshold"), 0.0,
					1.0, 0.025);

	/* GPU, CPU and performance Props */
	obs_property_t *p_use_gpu = obs_properties_add_list(
		props, "useGPU", obs_module_text("InferenceDevice"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p_use_gpu, obs_module_text("CPU"),
				     USEGPU_CPU);
#if defined(__linux__) && defined(__x86_64__)
	obs_property_list_add_string(p_use_gpu, obs_module_text("GPUTensorRT"),
				     USEGPU_TENSORRT);
#endif
#if _WIN32
	obs_property_list_add_string(p_use_gpu, obs_module_text("GPUDirectML"),
				     USEGPU_DML);
#endif
#if defined(__APPLE__)
	obs_property_list_add_string(p_use_gpu, obs_module_text("CoreML"),
				     USEGPU_COREML);
#endif

	obs_properties_add_int_slider(props, "numThreads",
				      obs_module_text("NumThreads"), 0, 8, 1);

	// add drop down option for model size: Small, Medium, Large
	obs_property_t *model_size = obs_properties_add_list(
		props, "model_size", obs_module_text("ModelSize"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(model_size, obs_module_text("SmallFast"),
				     "small");
	obs_property_list_add_string(model_size, obs_module_text("Medium"),
				     "medium");
	obs_property_list_add_string(model_size, obs_module_text("LargeSlow"),
				     "large");

	obs_properties_add_bool(props, "preview", obs_module_text("Preview"));

	// add dropdown selection for object category selection: "All", or COCO classes
	obs_property_t *object_category = obs_properties_add_list(
		props, "object_category", obs_module_text("ObjectCategory"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(object_category, obs_module_text("All"), -1);
	for (size_t i = 0; i < edgeyolo_cpp::COCO_CLASSES.size(); ++i) {
		const std::string &class_name = edgeyolo_cpp::COCO_CLASSES[i];
		// captilaze the first letter of the class name
		std::string class_name_cap = class_name;
		class_name_cap[0] = (char)std::toupper((int)class_name_cap[0]);
		obs_property_list_add_int(object_category,
					  class_name_cap.c_str(), (int)i);
	}

	// options group for masking
	obs_properties_t *masking_group = obs_properties_create();
	obs_property_t *masking_group_prop = obs_properties_add_group(
		props, "masking_group", obs_module_text("MaskingGroup"),
		OBS_GROUP_CHECKABLE, masking_group);

	// add callback to show/hide masking options
	obs_property_set_modified_callback(
		masking_group_prop, [](obs_properties_t *props_,
				       obs_property_t *, obs_data_t *settings) {
			const bool enabled =
				obs_data_get_bool(settings, "masking_group");
			obs_property_t *prop =
				obs_properties_get(props_, "masking_type");
			obs_property_set_visible(prop, enabled);
			obs_property_t *masking_color =
				obs_properties_get(props_, "masking_color");
			obs_property_t *masking_blur_radius =
				obs_properties_get(props_,
						   "masking_blur_radius");
			if (enabled) {
				const char *masking_type = obs_data_get_string(
					settings, "masking_type");
				if (strcmp(masking_type, "solid_color") == 0) {
					obs_property_set_visible(masking_color,
								 true);
					obs_property_set_visible(
						masking_blur_radius, false);
				} else if (strcmp(masking_type, "blur") == 0) {
					obs_property_set_visible(masking_color,
								 false);
					obs_property_set_visible(
						masking_blur_radius, true);
				} else {
					obs_property_set_visible(masking_color,
								 false);
					obs_property_set_visible(
						masking_blur_radius, false);
				}
			} else {
				obs_property_set_visible(masking_color, false);
				obs_property_set_visible(masking_blur_radius,
							 false);
			}
			return true;
		});

	// add masking options drop down selection: "None", "Solid color", "Blur", "Transparent"
	obs_property_t *masking_type = obs_properties_add_list(
		masking_group, "masking_type", obs_module_text("MaskingType"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(masking_type, obs_module_text("None"),
				     "none");
	obs_property_list_add_string(
		masking_type, obs_module_text("SolidColor"), "solid_color");
	obs_property_list_add_string(
		masking_type, obs_module_text("OutputMask"), "output_mask");
	obs_property_list_add_string(masking_type, obs_module_text("Blur"),
				     "blur");
	obs_property_list_add_string(
		masking_type, obs_module_text("Transparent"), "transparent");

	// add color picker for solid color masking
	obs_properties_add_color(masking_group, "masking_color",
				 obs_module_text("MaskingColor"));

	// add slider for blur radius
	obs_properties_add_int_slider(masking_group, "masking_blur_radius",
				      obs_module_text("MaskingBlurRadius"), 1,
				      30, 1);

	// add callback to show/hide blur radius and color picker
	obs_property_set_modified_callback(
		masking_type, [](obs_properties_t *props_, obs_property_t *,
				 obs_data_t *settings) {
			const char *masking_type_value =
				obs_data_get_string(settings, "masking_type");
			obs_property_t *masking_color =
				obs_properties_get(props_, "masking_color");
			obs_property_t *masking_blur_radius =
				obs_properties_get(props_,
						   "masking_blur_radius");
			if (strcmp(masking_type_value, "solid_color") == 0) {
				obs_property_set_visible(masking_color, true);
				obs_property_set_visible(masking_blur_radius,
							 false);
			} else if (strcmp(masking_type_value, "blur") == 0) {
				obs_property_set_visible(masking_color, false);
				obs_property_set_visible(masking_blur_radius,
							 true);
			} else {
				obs_property_set_visible(masking_color, false);
				obs_property_set_visible(masking_blur_radius,
							 false);
			}
			return true;
		});

	// add options group for tracking and zoom-follow options
	obs_properties_t *tracking_group_props = obs_properties_create();
	obs_property_t *tracking_group = obs_properties_add_group(
		props, "tracking_group",
		obs_module_text("TrackingZoomFollowGroup"), OBS_GROUP_CHECKABLE,
		tracking_group_props);

	// add callback to show/hide tracking options
	obs_property_set_modified_callback(
		tracking_group, [](obs_properties_t *props_, obs_property_t *,
				   obs_data_t *settings) {
			const bool enabled =
				obs_data_get_bool(settings, "tracking_group");
			for (auto prop_name : {"zoom_factor", "zoom_object"}) {
				obs_property_t *prop =
					obs_properties_get(props_, prop_name);
				obs_property_set_visible(prop, enabled);
			}
			return true;
		});

	// add zoom factor slider
	obs_properties_add_float_slider(tracking_group_props, "zoom_factor",
					obs_module_text("ZoomFactor"), 1.0,
					10.0, 0.1);

	// add object selection for zoom drop down: "Single", "All"
	obs_property_t *zoom_object = obs_properties_add_list(
		tracking_group_props, "zoom_object",
		obs_module_text("ZoomObject"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(zoom_object,
				     obs_module_text("SingleFirst"), "single");
	obs_property_list_add_string(zoom_object, obs_module_text("All"),
				     "all");

	// Add a informative text about the plugin
	// replace the placeholder with the current version
	// use std::regex_replace instead of QString::arg because the latter doesn't work on Linux
	std::string basic_info = std::regex_replace(
		PLUGIN_INFO_TEMPLATE, std::regex("%1"), PLUGIN_VERSION);
	// Check for update
	// if (get_latest_version() != nullptr) {
	// 	basic_info += std::regex_replace(
	// 		PLUGIN_INFO_TEMPLATE_UPDATE_AVAILABLE, std::regex("%1"),
	// 		get_latest_version());
	// }
	obs_properties_add_text(props, "info", basic_info.c_str(),
				OBS_TEXT_INFO);

	UNUSED_PARAMETER(data);
	return props;
}

void detect_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "advanced", false);
#if _WIN32
	obs_data_set_default_string(settings, "useGPU", USEGPU_DML);
#elif defined(__APPLE__)
	obs_data_set_default_string(settings, "useGPU", USEGPU_CPU);
#else
	// Linux
	obs_data_set_default_string(settings, "useGPU", USEGPU_CPU);
#endif
	obs_data_set_default_int(settings, "numThreads", 1);
	obs_data_set_default_bool(settings, "preview", false);
	obs_data_set_default_double(settings, "threshold", 0.5);
	obs_data_set_default_string(settings, "model_size", "small");
	obs_data_set_default_string(settings, "object_category", "all");
	obs_data_set_default_bool(settings, "masking_group", false);
	obs_data_set_default_string(settings, "masking_type", "none");
	obs_data_set_default_string(settings, "masking_color", "#000000");
	obs_data_set_default_int(settings, "masking_blur_radius", 0);
	obs_data_set_default_bool(settings, "tracking_group", false);
	obs_data_set_default_double(settings, "zoom_factor", 1.0);
	obs_data_set_default_string(settings, "zoom_object", "single");
}

void detect_filter_update(void *data, obs_data_t *settings)
{
	obs_log(LOG_INFO, "Detect filter updated");
	struct detect_filter *tf = reinterpret_cast<detect_filter *>(data);

	tf->isDisabled = true;

	tf->preview = obs_data_get_bool(settings, "preview");
	tf->conf_threshold = (float)obs_data_get_double(settings, "threshold");
	tf->objectCategory = (int)obs_data_get_int(settings, "object_category");
	tf->maskingEnabled = obs_data_get_bool(settings, "masking_group");
	tf->maskingType = obs_data_get_string(settings, "masking_type");
	tf->maskingColor = (int)obs_data_get_int(settings, "masking_color");
	tf->maskingBlurRadius =
		(int)obs_data_get_int(settings, "masking_blur_radius");
	tf->trackingEnabled = obs_data_get_bool(settings, "tracking_group");
	tf->zoomFactor = (float)obs_data_get_double(settings, "zoom_factor");
	tf->zoomObject = obs_data_get_string(settings, "zoom_object");

	obs_source_t *parent = obs_filter_get_parent(tf->source);
	if (tf->trackingEnabled) {
		obs_log(LOG_INFO, "Tracking enabled");
		// get the parent of the source
		// check if it has a crop/pad filter
		obs_source_t *crop_pad_filter = obs_source_get_filter_by_name(
			parent, "Detect Tracking");
		if (!crop_pad_filter) {
			// create a crop-pad filter
			crop_pad_filter = obs_source_create("crop_filter",
							    "Detect Tracking",
							    nullptr, nullptr);
			// add a crop/pad filter to the source
			// set the parent of the crop/pad filter to the parent of the source
			obs_source_filter_add(parent, crop_pad_filter);
		}
		tf->trackingFilter = crop_pad_filter;
	} else {
		obs_log(LOG_INFO, "Tracking disabled");
		// remove the crop/pad filter
		obs_source_t *crop_pad_filter = obs_source_get_filter_by_name(
			parent, "Detect Tracking");
		if (crop_pad_filter) {
			obs_source_filter_remove(parent, crop_pad_filter);
		}
		tf->trackingFilter = nullptr;
	}

	const std::string newUseGpu = obs_data_get_string(settings, "useGPU");
	const uint32_t newNumThreads =
		(uint32_t)obs_data_get_int(settings, "numThreads");
	const std::string newModelSize =
		obs_data_get_string(settings, "model_size");

	if (tf->useGPU != newUseGpu || tf->numThreads != newNumThreads ||
	    tf->modelSize != newModelSize) {
		// lock modelMutex
		std::unique_lock<std::mutex> lock(tf->modelMutex);

		char *modelFilepath_rawPtr = nullptr;
		if (newModelSize == "small") {
			modelFilepath_rawPtr = obs_module_file(
				"models/edgeyolo_tiny_lrelu_coco_256x416.onnx");
		} else if (newModelSize == "medium") {
			modelFilepath_rawPtr = obs_module_file(
				"models/edgeyolo_tiny_lrelu_coco_480x800.onnx");
		} else {
			modelFilepath_rawPtr = obs_module_file(
				"models/edgeyolo_tiny_lrelu_coco_736x1280.onnx");
		}

		if (modelFilepath_rawPtr == nullptr) {
			obs_log(LOG_ERROR,
				"Unable to get model filename from plugin.");
			return;
		}

		std::string modelFilepath_s(modelFilepath_rawPtr);

#if _WIN32
		int outLength = MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED,
						    modelFilepath_rawPtr, -1,
						    nullptr, 0);
		tf->modelFilepath = std::wstring(outLength, L'\0');
		MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED,
				    modelFilepath_rawPtr, -1,
				    tf->modelFilepath.data(), outLength);
#else
		tf->modelFilepath = std::string(modelFilepath_rawPtr);
#endif

		// Re-initialize model if it's not already the selected one or switching inference device
		tf->useGPU = newUseGpu;
		tf->numThreads = newNumThreads;
		tf->modelSize = newModelSize;

		// parameters
		int onnxruntime_device_id_ = 0;
		bool onnxruntime_use_parallel_ = true;
		float nms_th_ = 0.45f;
		int num_classes_ = (int)edgeyolo_cpp::COCO_CLASSES.size();

		// Load model
		try {
			tf->edgeyolo = std::make_unique<
				edgeyolo_cpp::EdgeYOLOONNXRuntime>(
				tf->modelFilepath, tf->numThreads,
				tf->numThreads, tf->useGPU,
				onnxruntime_device_id_,
				onnxruntime_use_parallel_, nms_th_,
				tf->conf_threshold, num_classes_);
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "Failed to load model: %s",
				e.what());
			// disable filter
			tf->isDisabled = true;
			tf->edgeyolo.reset();
			return;
		}
	}

	// update threshold on edgeyolo
	tf->edgeyolo->setBBoxConfThresh(tf->conf_threshold);

	// Log the currently selected options
	obs_log(LOG_INFO, "Detect Filter Options:");
	// name of the source that the filter is attached to
	obs_log(LOG_INFO, "  Source: %s", obs_source_get_name(tf->source));
	obs_log(LOG_INFO, "  Inference Device: %s", tf->useGPU.c_str());
	obs_log(LOG_INFO, "  Num Threads: %d", tf->numThreads);
	obs_log(LOG_INFO, "  Preview: %s", tf->preview ? "true" : "false");
	obs_log(LOG_INFO, "  Threshold: %.2f", tf->conf_threshold);
	obs_log(LOG_INFO, "  Object Category: %s",
		obs_data_get_string(settings, "object_category"));
	obs_log(LOG_INFO, "  Masking Enabled: %s",
		obs_data_get_bool(settings, "masking_group") ? "true"
							     : "false");
	obs_log(LOG_INFO, "  Masking Type: %s",
		obs_data_get_string(settings, "masking_type"));
	obs_log(LOG_INFO, "  Masking Color: %s",
		obs_data_get_string(settings, "masking_color"));
	obs_log(LOG_INFO, "  Masking Blur Radius: %d",
		obs_data_get_int(settings, "masking_blur_radius"));
	obs_log(LOG_INFO, "  Tracking Enabled: %s",
		obs_data_get_bool(settings, "tracking_group") ? "true"
							      : "false");
	obs_log(LOG_INFO, "  Zoom Factor: %.2f",
		obs_data_get_double(settings, "zoom_factor"));
	obs_log(LOG_INFO, "  Zoom Object: %s",
		obs_data_get_string(settings, "zoom_object"));
	obs_log(LOG_INFO, "  Disabled: %s", tf->isDisabled ? "true" : "false");
#ifdef _WIN32
	obs_log(LOG_INFO, "  Model file path: %S", tf->modelFilepath.c_str());
#else
	obs_log(LOG_INFO, "  Model file path: %s", tf->modelFilepath.c_str());
#endif

	// enable
	tf->isDisabled = false;
}

void detect_filter_activate(void *data)
{
	obs_log(LOG_INFO, "Detect filter activated");
	struct detect_filter *tf = reinterpret_cast<detect_filter *>(data);
	tf->isDisabled = false;
}

void detect_filter_deactivate(void *data)
{
	obs_log(LOG_INFO, "Detect filter deactivated");
	struct detect_filter *tf = reinterpret_cast<detect_filter *>(data);
	tf->isDisabled = true;
}

/**                   FILTER CORE                     */

void *detect_filter_create(obs_data_t *settings, obs_source_t *source)
{
	obs_log(LOG_INFO, "Detect filter created");
	void *data = bmalloc(sizeof(struct detect_filter));
	struct detect_filter *tf = new (data) detect_filter();

	tf->source = source;
	tf->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	tf->effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	char *kawaseBlurEffectPath = obs_module_file(KAWASE_BLUR_EFFECT_PATH);
	if (!kawaseBlurEffectPath) {
		obs_log(LOG_ERROR, "Failed to get Kawase Blur effect path");
		tf->isDisabled = true;
		return tf;
	}
	char *maskingEffectPath = obs_module_file(MASKING_EFFECT_PATH);
	if (!maskingEffectPath) {
		obs_log(LOG_ERROR, "Failed to get Kawase Blur effect path");
		tf->isDisabled = true;
		bfree(kawaseBlurEffectPath);
		return tf;
	}

	obs_enter_graphics();
	gs_effect_destroy(tf->kawaseBlurEffect);
	tf->kawaseBlurEffect = nullptr;
	char *error = nullptr;
	tf->kawaseBlurEffect =
		gs_effect_create_from_file(kawaseBlurEffectPath, &error);
	bfree(kawaseBlurEffectPath);
	if (!tf->kawaseBlurEffect || error) {
		obs_log(LOG_ERROR, "Failed to load Kawase Blur effect: %s",
			error);
	}
	gs_effect_destroy(tf->maskingEffect);
	tf->maskingEffect = nullptr;
	tf->maskingEffect =
		gs_effect_create_from_file(maskingEffectPath, &error);
	bfree(maskingEffectPath);
	if (!tf->maskingEffect || error) {
		obs_log(LOG_ERROR, "Failed to load masking effect: %s", error);
	}
	obs_leave_graphics();

	detect_filter_update(tf, settings);

	return tf;
}

void detect_filter_destroy(void *data)
{
	obs_log(LOG_INFO, "Detect filter destroyed");

	struct detect_filter *tf = reinterpret_cast<detect_filter *>(data);

	if (tf) {
		tf->isDisabled = true;

		obs_enter_graphics();
		gs_texrender_destroy(tf->texrender);
		if (tf->stagesurface) {
			gs_stagesurface_destroy(tf->stagesurface);
		}
		obs_leave_graphics();
		tf->~detect_filter();
		bfree(tf);
	}
}

void detect_filter_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct detect_filter *tf = reinterpret_cast<detect_filter *>(data);

	if (tf->isDisabled) {
		return;
	}

	if (!obs_source_enabled(tf->source)) {
		return;
	}

	if (!tf->edgeyolo) {
		obs_log(LOG_ERROR, "Model is not initialized");
		return;
	}

	cv::Mat imageBGRA;
	{
		std::unique_lock<std::mutex> lock(tf->inputBGRALock,
						  std::try_to_lock);
		if (!lock.owns_lock()) {
			// No data to process
			return;
		}
		if (tf->inputBGRA.empty()) {
			// No data to process
			return;
		}
		imageBGRA = tf->inputBGRA.clone();
	}

	cv::Mat frame;
	cv::cvtColor(imageBGRA, frame, cv::COLOR_BGRA2BGR);
	std::vector<edgeyolo_cpp::Object> objects;

	try {
		std::unique_lock<std::mutex> lock(tf->modelMutex);
		objects = tf->edgeyolo->inference(frame);
	} catch (const Ort::Exception &e) {
		obs_log(LOG_ERROR, "ONNXRuntime Exception: %s", e.what());
		// TODO: Fall back to CPU if it makes sense
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "%s", e.what());
	}

	if (tf->objectCategory != -1) {
		std::vector<edgeyolo_cpp::Object> filtered_objects;
		for (const edgeyolo_cpp::Object &obj : objects) {
			if (obj.label == tf->objectCategory) {
				filtered_objects.push_back(obj);
			}
		}
		objects = filtered_objects;
	}

	if (tf->preview || tf->maskingEnabled) {
		if (tf->preview && objects.size() > 0) {
			edgeyolo_cpp::utils::draw_objects(frame, objects);
		}
		if (tf->maskingEnabled) {
			cv::Mat mask = cv::Mat::zeros(frame.size(), CV_8UC1);
			for (const edgeyolo_cpp::Object &obj : objects) {
				cv::rectangle(mask, obj.rect, cv::Scalar(255),
					      -1);
			}
			std::lock_guard<std::mutex> lock(tf->outputLock);
			mask.copyTo(tf->outputMask);
		}

		std::lock_guard<std::mutex> lock(tf->outputLock);
		cv::cvtColor(frame, tf->outputPreviewBGRA, cv::COLOR_BGR2BGRA);
	}

	if (tf->trackingEnabled && tf->trackingFilter) {
		cv::Rect2f boundingBox =
			cv::Rect2f(0, 0, (float)frame.cols, (float)frame.rows);
		// get location of the objects
		if (tf->zoomObject == "single") {
			if (objects.size() > 0) {
				boundingBox = objects[0].rect;
			}
		} else {
			// get the bounding box of all objects
			if (objects.size() > 0) {
				boundingBox = objects[0].rect;
				for (const edgeyolo_cpp::Object &obj :
				     objects) {
					boundingBox |= obj.rect;
				}
			}
		}
		bool lostTracking = objects.size() == 0;
		// the zooming box should maintain the aspect ratio of the image
		// with the tf->zoomFactor controlling the effective buffer around the bounding box
		// the bounding box is the center of the zooming box
		float frameAspectRatio = (float)frame.cols / (float)frame.rows;
		// calculate an aspect ratio box around the object using its height
		float boxHeight = boundingBox.height;
		// calculate the zooming box size
		// when the zoom factor is 1, the zooming box is the same size as the bounding box
		// when the zoom factor is 10, the zooming box is the same size of the image
		float dh = (float)frame.rows - boxHeight;
		float buffer = dh * ((tf->zoomFactor - 1) / 9);
		float zh = boxHeight + buffer;
		float zw = zh * frameAspectRatio;
		// calculate the top left corner of the zooming box
		float zx = boundingBox.x - (zw - boundingBox.width) / 2.0f;
		float zy = boundingBox.y - (zh - boundingBox.height) / 2.0f;

		if (tf->trackingRect.width == 0) {
			// initialize the trackingRect
			tf->trackingRect = cv::Rect2f(zx, zy, zw, zh);
		} else {
			// interpolate the zooming box to tf->trackingRect
			// the interpolation factor is (lostTracking) ? 0.1 : 0.5  to make the zooming box move smoothly
			float factor = lostTracking ? 0.01f : 0.05f;
			tf->trackingRect.x = tf->trackingRect.x +
					     factor * (zx - tf->trackingRect.x);
			tf->trackingRect.y = tf->trackingRect.y +
					     factor * (zy - tf->trackingRect.y);
			tf->trackingRect.width =
				tf->trackingRect.width +
				factor * (zw - tf->trackingRect.width);
			tf->trackingRect.height =
				tf->trackingRect.height +
				factor * (zh - tf->trackingRect.height);
		}

		// get the settings of the crop/pad filter
		obs_data_t *crop_pad_settings =
			obs_source_get_settings(tf->trackingFilter);
		obs_data_set_int(crop_pad_settings, "left",
				 (int)tf->trackingRect.x);
		obs_data_set_int(crop_pad_settings, "top",
				 (int)tf->trackingRect.y);
		// right = image width - (zx + zw)
		obs_data_set_int(
			crop_pad_settings, "right",
			(int)((float)frame.cols -
			      (tf->trackingRect.x + tf->trackingRect.width)));
		// bottom = image height - (zy + zh)
		obs_data_set_int(
			crop_pad_settings, "bottom",
			(int)((float)frame.rows -
			      (tf->trackingRect.y + tf->trackingRect.height)));
		// apply the settings
		obs_source_update(tf->trackingFilter, crop_pad_settings);
		obs_data_release(crop_pad_settings);
	}
}

void detect_filter_video_render(void *data, gs_effect_t *_effect)
{
	UNUSED_PARAMETER(_effect);

	struct detect_filter *tf = reinterpret_cast<detect_filter *>(data);

	if (tf->isDisabled) {
		if (tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		return;
	}

	uint32_t width, height;
	if (!getRGBAFromStageSurface(tf, width, height)) {
		if (tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		return;
	}

	// if preview is enabled, render the image
	if (tf->preview || tf->maskingEnabled) {
		cv::Mat outputBGRA, outputMask;
		{
			// lock the outputLock mutex
			std::lock_guard<std::mutex> lock(tf->outputLock);
			if (tf->outputPreviewBGRA.empty()) {
				obs_log(LOG_ERROR, "Preview image is empty");
				if (tf->source) {
					obs_source_skip_video_filter(
						tf->source);
				}
				return;
			}
			if ((uint32_t)tf->outputPreviewBGRA.cols != width ||
			    (uint32_t)tf->outputPreviewBGRA.rows != height) {
				if (tf->source) {
					obs_source_skip_video_filter(
						tf->source);
				}
				return;
			}
			outputBGRA = tf->outputPreviewBGRA.clone();
			outputMask = tf->outputMask.clone();
		}

		gs_texture_t *tex = gs_texture_create(
			width, height, GS_BGRA, 1,
			(const uint8_t **)&outputBGRA.data, 0);
		gs_texture_t *maskTexture = nullptr;
		std::string technique_name = "Draw";
		gs_eparam_t *imageParam =
			gs_effect_get_param_by_name(tf->maskingEffect, "image");
		gs_eparam_t *maskParam = gs_effect_get_param_by_name(
			tf->maskingEffect, "focalmask");
		gs_eparam_t *maskColorParam =
			gs_effect_get_param_by_name(tf->maskingEffect, "color");

		if (tf->maskingEnabled) {
			maskTexture = gs_texture_create(
				width, height, GS_R8, 1,
				(const uint8_t **)&outputMask.data, 0);
			gs_effect_set_texture(maskParam, maskTexture);
			if (tf->maskingType == "output_mask") {
				technique_name = "DrawMask";
			} else if (tf->maskingType == "blur") {
				gs_texture_destroy(tex);
				tex = blur_image(tf, width, height,
						 maskTexture);
			} else if (tf->maskingType == "transparent") {
				technique_name = "DrawSolidColor";
				gs_effect_set_color(maskColorParam, 0);
			} else if (tf->maskingType == "solid_color") {
				technique_name = "DrawSolidColor";
				gs_effect_set_color(maskColorParam,
						    tf->maskingColor);
			}
		}

		gs_effect_set_texture(imageParam, tex);

		while (gs_effect_loop(tf->maskingEffect,
				      technique_name.c_str())) {
			gs_draw_sprite(tex, 0, 0, 0);
		}

		gs_texture_destroy(tex);
		gs_texture_destroy(maskTexture);
	} else {
		obs_source_skip_video_filter(tf->source);
	}
	return;
}
