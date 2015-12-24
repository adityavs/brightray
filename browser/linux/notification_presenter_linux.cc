// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Copyright (c) 2013 Patrick Reynolds <piki@github.com>. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE-CHROMIUM file.

#include "browser/linux/notification_presenter_linux.h"

#include "base/bind.h"
#include "base/logging.h"
#include "base/files/file_enumerator.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "common/application_info.h"
#include "content/public/browser/desktop_notification_delegate.h"
#include "content/public/common/platform_notification_data.h"
#include "chrome/browser/ui/libgtk2ui/skia_utils_gtk2.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace brightray {

namespace {

bool unity_has_result = false;
bool unity_result = false;

bool UnityIsRunning() {
  if (getenv("ELECTRON_USE_UBUNTU_NOTIFIER"))
    return true;

  if (unity_has_result)
    return unity_result;

  unity_has_result = true;

  // Look for the presence of libunity as our hint that we're under Ubuntu.
  base::FileEnumerator enumerator(base::FilePath("/usr/lib"),
                                  false, base::FileEnumerator::FILES);
  base::FilePath haystack;
  while (!((haystack = enumerator.Next()).empty())) {
    if (base::StartsWith(haystack.value(), "/usr/lib/libunity-", base::CompareCase::SENSITIVE)) {
      unity_result = true;
      break;
    }
  }

  return unity_result;
}

void log_and_clear_error(GError* error, const char* context) {
  LOG(ERROR) << context
             << ": domain=" << error->domain
             << " code=" << error->code
             << " message=\"" << error->message << '"';
  g_error_free(error);
}

content::DesktopNotificationDelegate* GetDelegateFromNotification(
    NotifyNotification* notification) {
  return static_cast<content::DesktopNotificationDelegate*>(
      g_object_get_data(G_OBJECT(notification), "delegate"));
}

}  // namespace

// static
NotificationPresenter* NotificationPresenter::Create() {
  scoped_ptr<NotificationPresenterLinux> presenter(
      new NotificationPresenterLinux);
  if (presenter->Init())
    return presenter.release();
  else
    return nullptr;
}

NotificationPresenterLinux::NotificationPresenterLinux()
    : notifications_(nullptr) {
}

NotificationPresenterLinux::~NotificationPresenterLinux() {
  // unref any outstanding notifications, and then free the list.
  if (notifications_)
    g_list_free_full(notifications_, g_object_unref);
}

bool NotificationPresenterLinux::Init() {
  if (!libnotify_loader_.Load("libnotify.so.4") &&
      !libnotify_loader_.Load("libnotify.so.1") &&
      !libnotify_loader_.Load("libnotify.so")) {
    return false;
  }
  if (!libnotify_loader_.notify_is_initted() &&
      !libnotify_loader_.notify_init(GetApplicationName().c_str())) {
    return false;
  }
  return true;
}

void NotificationPresenterLinux::ShowNotification(
    const content::PlatformNotificationData& data,
    const SkBitmap& icon,
    scoped_ptr<content::DesktopNotificationDelegate> delegate_ptr,
    base::Closure* cancel_callback) {
  std::string title = base::UTF16ToUTF8(data.title);
  std::string body = base::UTF16ToUTF8(data.body);
  NotifyNotification* notification = libnotify_loader_.notify_notification_new(
      title.c_str(), body.c_str(), nullptr);

  content::DesktopNotificationDelegate* delegate = delegate_ptr.release();

  g_object_set_data_full(G_OBJECT(notification), "delegate", delegate, operator delete);
  g_signal_connect(notification, "closed", G_CALLBACK(OnNotificationClosedThunk), this);

  // NB: On Unity, adding a notification action will cause the notification
  // to display as a modal dialog box. Testing for distros that have "Unity
  // Zen Nature" is difficult, we will test for the presence of the indicate
  // dbus service
  if (!UnityIsRunning()) {
    libnotify_loader_.notify_notification_add_action(
        notification, "default", "View", OnNotificationViewThunk, this, nullptr);
  }

  if (!icon.drawsNothing()) {
    GdkPixbuf* pixbuf = libgtk2ui::GdkPixbufFromSkBitmap(icon);
    libnotify_loader_.notify_notification_set_image_from_pixbuf(
        notification, pixbuf);
    libnotify_loader_.notify_notification_set_timeout(
        notification, NOTIFY_EXPIRES_DEFAULT);
    g_object_unref(pixbuf);
  }

  GError* error = nullptr;
  libnotify_loader_.notify_notification_show(notification, &error);
  if (error) {
    log_and_clear_error(error, "notify_notification_show");
    g_object_unref(notification);
    return;
  }

  notifications_ = g_list_append(notifications_, notification);
  delegate->NotificationDisplayed();

  if (cancel_callback)
    *cancel_callback = base::Bind(
        &NotificationPresenterLinux::CancelNotification,
        base::Unretained(this),
        notification);
}

void NotificationPresenterLinux::CancelNotification(NotifyNotification* notification) {
  GError* error = nullptr;
  libnotify_loader_.notify_notification_close(notification, &error);
  if (error)
    log_and_clear_error(error, "notify_notification_close");

  GetDelegateFromNotification(notification)->NotificationClosed();
  DeleteNotification(notification);
}

void NotificationPresenterLinux::DeleteNotification(NotifyNotification* notification) {
  notifications_ = g_list_remove(notifications_, notification);
  g_object_unref(notification);
}

void NotificationPresenterLinux::OnNotificationClosed(NotifyNotification* notification) {
  if (!notification)
    return;
  GetDelegateFromNotification(notification)->NotificationClosed();
  DeleteNotification(notification);
}

void NotificationPresenterLinux::OnNotificationView(
    NotifyNotification* notification, char* action) {
  if (!notification)
    return;
  GetDelegateFromNotification(notification)->NotificationClick();
  DeleteNotification(notification);
}

}  // namespace brightray
