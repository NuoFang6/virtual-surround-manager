#pragma once

#include <QObject>
#include <QString>

class QAction;
class QActionGroup;
class FrontendManager;
class QMenu;
class QQuickWindow;
class QSystemTrayIcon;

class TrayIcon : public QObject {
    Q_OBJECT

  public:
    explicit TrayIcon(QObject *parent = nullptr);
    ~TrayIcon() override;

    void set_frontend_manager(FrontendManager *frontend_manager);
    void setup(const QString &icon_name);
    void set_main_window(QQuickWindow *window);
    void set_visibility(bool visible);
    void update_profile_menu();

    QSystemTrayIcon *get_tray_icon() const { return m_tray_icon; }

    Q_INVOKABLE void hide_main_window();
    Q_INVOKABLE bool is_visible();
    Q_INVOKABLE void show_main_window();
    Q_INVOKABLE void quit_application();
    Q_INVOKABLE void toggle_virtual_surround();
    Q_INVOKABLE void update_toggle_action_state();

  private:
    void on_profile_selected();
    void update_ui_state();

    QSystemTrayIcon *m_tray_icon = nullptr;
    QMenu *m_menu = nullptr;
    QMenu *m_profile_menu = nullptr;
    QActionGroup *m_profile_action_group = nullptr;
    QAction *m_toggle_action = nullptr;

    FrontendManager *m_frontend_manager = nullptr;
    QQuickWindow *m_main_window = nullptr;
};
