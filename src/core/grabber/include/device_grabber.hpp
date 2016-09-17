#pragma once

#include "apple_hid_usage_tables.hpp"
#include "console_user_client.hpp"
#include "constants.hpp"
#include "event_manipulator.hpp"
#include "hid_system_client.hpp"
#include "human_interface_device.hpp"
#include "iokit_utility.hpp"
#include "logger.hpp"
#include "manipulator.hpp"
#include "types.hpp"
#include <thread>
#include <time.h>

class device_grabber final {
public:
  device_grabber(const device_grabber&) = delete;

  device_grabber(manipulator::event_manipulator& event_manipulator) : event_manipulator_(event_manipulator),
                                                                      hid_system_client_(logger::get_logger()),
                                                                      grab_timer_(0),
                                                                      grab_retry_count_(0),
                                                                      grabbed_(false),
                                                                      console_user_client_() {
    manager_ = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager_) {
      logger::get_logger().error("{0}: failed to IOHIDManagerCreate", __PRETTY_FUNCTION__);
      return;
    }

    auto device_matching_dictionaries = iokit_utility::create_device_matching_dictionaries({
        std::make_pair(kHIDPage_GenericDesktop, kHIDUsage_GD_Keyboard),
        // std::make_pair(kHIDPage_Consumer, kHIDUsage_Csmr_ConsumerControl),
        // std::make_pair(kHIDPage_GenericDesktop, kHIDUsage_GD_Mouse),
    });
    if (device_matching_dictionaries) {
      IOHIDManagerSetDeviceMatchingMultiple(manager_, device_matching_dictionaries);
      CFRelease(device_matching_dictionaries);

      IOHIDManagerRegisterDeviceMatchingCallback(manager_, static_device_matching_callback, this);
      IOHIDManagerRegisterDeviceRemovalCallback(manager_, static_device_removal_callback, this);

      IOHIDManagerScheduleWithRunLoop(manager_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    }
  }

  ~device_grabber(void) {
    cancel_grab_timer();

    if (manager_) {
      IOHIDManagerUnscheduleFromRunLoop(manager_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
      CFRelease(manager_);
      manager_ = nullptr;
    }
  }

  void clear_simple_modifications(void) {
    std::lock_guard<std::mutex> guard(simple_modifications_mutex_);

    simple_modifications_.clear();
  }

  void add_simple_modification(krbn::key_code from_key_code, krbn::key_code to_key_code) {
    std::lock_guard<std::mutex> guard(simple_modifications_mutex_);

    simple_modifications_[from_key_code] = to_key_code;
  }

  void grab_devices(void) {
    auto __block last_warning_message_time = ::time(nullptr) - 1;

    // we run grab_devices and ungrab_devices in the main queue.
    dispatch_async(dispatch_get_main_queue(), ^{
      cancel_grab_timer();

      // ----------------------------------------
      // setup grab_timer_

      grab_retry_count_ = 0;

      grab_timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
      dispatch_source_set_timer(grab_timer_, dispatch_time(DISPATCH_TIME_NOW, 0.1 * NSEC_PER_SEC), 0.1 * NSEC_PER_SEC, 0);
      dispatch_source_set_event_handler(grab_timer_, ^{
        if (grabbed_) {
          return;
        }

        const char* warning_message = nullptr;

        if (!event_manipulator_.is_ready()) {
          warning_message = "event_manipulator_ is not ready. Please wait for a while.";
        }

        if (get_all_devices_pressed_keys_count() > 0) {
          warning_message = "There are pressed down keys in some devices. Please release them.";
        }

        if (warning_message) {
          auto time = ::time(nullptr);
          if (last_warning_message_time != time) {
            last_warning_message_time = time;
            logger::get_logger().warn(warning_message);
            return;
          }
        }

        // ----------------------------------------
        // grab devices

        grabbed_ = true;

        for (auto&& it : hids_) {
          grab(*(it.second));
          (it.second)->clear_changed_keys();
          (it.second)->clear_pressed_keys();
        }

        modifier_flag_manager_.reset();
        hid_system_client_.set_caps_lock_state(false);

        logger::get_logger().info("devices are grabbed");

        cancel_grab_timer();
      });
      dispatch_resume(grab_timer_);
    });
  }

  void ungrab_devices(void) {
    // we run grab_devices and ungrab_devices in the main queue.
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!grabbed_) {
        return;
      }

      grabbed_ = false;

      cancel_grab_timer();

      for (auto&& it : hids_) {
        ungrab(*(it.second));
        (it.second)->clear_changed_keys();
        (it.second)->clear_pressed_keys();
      }

      modifier_flag_manager_.reset();
      hid_system_client_.set_caps_lock_state(false);
      console_user_client_.stop_key_repeat();

      logger::get_logger().info("devices are ungrabbed");
    });
  }

  void post_connect_ack(void) {
    console_user_client_.connect_ack();
  }

  void set_caps_lock_led_state(krbn::led_state state) {
    for (const auto& it : hids_) {
      (it.second)->set_caps_lock_led_state(state);
    }
  }

private:
  static void static_device_matching_callback(void* _Nullable context, IOReturn result, void* _Nullable sender, IOHIDDeviceRef _Nonnull device) {
    if (result != kIOReturnSuccess) {
      return;
    }

    auto self = static_cast<device_grabber*>(context);
    if (!self) {
      return;
    }

    self->device_matching_callback(device);
  }

  void device_matching_callback(IOHIDDeviceRef _Nonnull device) {
    if (!device) {
      return;
    }

    hids_[device] = std::make_unique<human_interface_device>(logger::get_logger(), device);
    auto& dev = hids_[device];

    auto manufacturer = dev->get_manufacturer();
    auto product = dev->get_product();
    auto vendor_id = dev->get_vendor_id();
    auto product_id = dev->get_product_id();
    auto location_id = dev->get_location_id();
    auto serial_number = dev->get_serial_number();

    logger::get_logger().info("matching device: "
                              "manufacturer:{1}, "
                              "product:{2}, "
                              "vendor_id:{3:#x}, "
                              "product_id:{4:#x}, "
                              "location_id:{5:#x}, "
                              "serial_number:{6} "
                              "registry_entry_id:{7} "
                              "@ {0}",
                              __PRETTY_FUNCTION__,
                              manufacturer ? *manufacturer : "",
                              product ? *product : "",
                              vendor_id ? *vendor_id : 0,
                              product_id ? *product_id : 0,
                              location_id ? *location_id : 0,
                              serial_number ? *serial_number : "",
                              dev->get_registry_entry_id());

    if (grabbed_) {
      grab(*dev);
    } else {
      observe(*dev);
    }
  }

  static void static_device_removal_callback(void* _Nullable context, IOReturn result, void* _Nullable sender, IOHIDDeviceRef _Nonnull device) {
    if (result != kIOReturnSuccess) {
      return;
    }

    auto self = static_cast<device_grabber*>(context);
    if (!self) {
      return;
    }

    self->device_removal_callback(device);
  }

  void device_removal_callback(IOHIDDeviceRef _Nonnull device) {
    if (!device) {
      return;
    }

    auto it = hids_.find(device);
    if (it != hids_.end()) {
      auto& dev = it->second;
      if (dev) {
        auto vendor_id = dev->get_vendor_id();
        auto product_id = dev->get_product_id();
        auto location_id = dev->get_location_id();
        logger::get_logger().info("removal device: "
                                  "vendor_id:{1:#x}, "
                                  "product_id:{2:#x}, "
                                  "location_id:{3:#x} "
                                  "@ {0}",
                                  __PRETTY_FUNCTION__,
                                  vendor_id ? *vendor_id : 0,
                                  product_id ? *product_id : 0,
                                  location_id ? *location_id : 0);

        hids_.erase(it);
      }
    }
  }

  void observe(human_interface_device& hid) {
    human_interface_device::value_callback callback;
    hid.observe(callback);
  }

  void unobserve(human_interface_device& hid) {
    hid.unobserve();
  }

  void grab(human_interface_device& hid) {
    auto manufacturer = hid.get_manufacturer();
    if (manufacturer && *manufacturer == "pqrs.org") {
      return;
    }

    unobserve(hid);

    // seize device
    hid.grab(std::bind(&device_grabber::value_callback,
                       this,
                       std::placeholders::_1,
                       std::placeholders::_2,
                       std::placeholders::_3,
                       std::placeholders::_4,
                       std::placeholders::_5,
                       std::placeholders::_6));

    // set keyboard led
    auto caps_lock_state = hid_system_client_.get_caps_lock_state();
    if (caps_lock_state && *caps_lock_state) {
      hid.set_caps_lock_led_state(krbn::led_state::on);
    } else {
      hid.set_caps_lock_led_state(krbn::led_state::off);
    }
  }

  void ungrab(human_interface_device& hid) {
    auto manufacturer = hid.get_manufacturer();
    if (manufacturer && *manufacturer == "pqrs.org") {
      return;
    }

    hid.ungrab();
    observe(hid);
  }

  void value_callback(human_interface_device& device,
                      IOHIDValueRef _Nonnull value,
                      IOHIDElementRef _Nonnull element,
                      uint32_t usage_page,
                      uint32_t usage,
                      CFIndex integer_value) {
    if (!grabbed_) {
      return;
    }

    auto device_registry_entry_id = manipulator::device_registry_entry_id(device.get_registry_entry_id());

    switch (usage_page) {
    case kHIDPage_KeyboardOrKeypad:
      if (kHIDUsage_KeyboardErrorUndefined < usage && usage < kHIDUsage_Keyboard_Reserved) {
        bool pressed = integer_value;
        event_manipulator_.handle_keyboard_event(device_registry_entry_id, krbn::key_code(usage), pressed);
      }
      break;

    case kHIDPage_AppleVendorTopCase:
      if (usage == kHIDUsage_AV_TopCase_KeyboardFn) {
        bool pressed = integer_value;
        event_manipulator_.handle_keyboard_event(device_registry_entry_id, krbn::key_code::vk_fn_modifier, pressed);
      }
      break;

    default:
      break;
    }
  }

  size_t get_all_devices_pressed_keys_count(void) const {
    size_t total = 0;
    for (const auto& it : hids_) {
      total += (it.second)->get_pressed_keys_count();
    }
    return total;
  }

  void cancel_grab_timer(void) {
    if (grab_timer_) {
      dispatch_source_cancel(grab_timer_);
      dispatch_release(grab_timer_);
      grab_timer_ = 0;
    }
  }

  manipulator::event_manipulator& event_manipulator_;
  hid_system_client hid_system_client_;
  IOHIDManagerRef _Nullable manager_;
  std::unordered_map<IOHIDDeviceRef, std::unique_ptr<human_interface_device>> hids_;
  dispatch_source_t _Nullable grab_timer_;
  size_t grab_retry_count_;
  bool grabbed_;

  manipulator::modifier_flag_manager modifier_flag_manager_;

  std::unordered_map<krbn::key_code, krbn::key_code> simple_modifications_;
  std::mutex simple_modifications_mutex_;

  console_user_client console_user_client_;
};
