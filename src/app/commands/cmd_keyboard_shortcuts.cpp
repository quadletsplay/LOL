// Aseprite
// Copyright (C) 2018-2023  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/app.h"
#include "app/app_menus.h"
#include "app/commands/command.h"
#include "app/context.h"
#include "app/file_selector.h"
#include "app/i18n/strings.h"
#include "app/match_words.h"
#include "app/modules/gui.h"
#include "app/resource_finder.h"
#include "app/tools/tool.h"
#include "app/tools/tool_box.h"
#include "app/ui/app_menuitem.h"
#include "app/ui/keyboard_shortcuts.h"
#include "app/ui/search_entry.h"
#include "app/ui/select_accelerator.h"
#include "app/ui/separator_in_view.h"
#include "app/ui/skin/skin_theme.h"
#include "base/fs.h"
#include "base/pi.h"
#include "base/scoped_value.h"
#include "base/split_string.h"
#include "base/string.h"
#include "fmt/format.h"
#include "ui/alert.h"
#include "ui/fit_bounds.h"
#include "ui/graphics.h"
#include "ui/listitem.h"
#include "ui/message.h"
#include "ui/paint_event.h"
#include "ui/resize_event.h"
#include "ui/separator.h"
#include "ui/size_hint_event.h"
#include "ui/splitter.h"
#include "ui/system.h"

#include "keyboard_shortcuts.xml.h"

#include <algorithm>
#include <map>
#include <memory>

namespace app {

using namespace skin;
using namespace tools;
using namespace ui;

namespace {

using MenuKeys = std::map<AppMenuItem*, KeyPtr>;

class HeaderSplitter : public Splitter {
public:
  HeaderSplitter() : Splitter(Splitter::ByPixel, HORIZONTAL) {
  }
  void onPositionChange() override {
    Splitter::onPositionChange();

    Widget* p = parent();
    while (p && p->type() != kViewWidget)
      p = p->parent();
    if (p)
      p->layout();
  }
};

class HeaderItem : public ListItem {
public:
  HeaderItem()
    : m_actionLabel(Strings::keyboard_shortcuts_header_action())
    , m_keyLabel(Strings::keyboard_shortcuts_header_key())
    , m_contextLabel(Strings::keyboard_shortcuts_header_context()) {
    setBorder(gfx::Border(0));

    auto theme = SkinTheme::get(this);
    m_actionLabel.setStyle(theme->styles.listHeaderLabel());
    m_keyLabel.setStyle(theme->styles.listHeaderLabel());
    m_contextLabel.setStyle(theme->styles.listHeaderLabel());

    gfx::Size displaySize = display()->size();
    m_splitter1.setPosition(displaySize.w*3/4 * 4/10);
    m_splitter2.setPosition(displaySize.w*3/4 * 2/10);

    addChild(&m_splitter1);
    m_splitter1.addChild(&m_actionLabel);
    m_splitter1.addChild(&m_splitter2);
    m_splitter2.addChild(&m_keyLabel);
    m_splitter2.addChild(&m_contextLabel);
  }

  int keyXPos() const {
    return m_keyLabel.bounds().x - bounds().x;
  }

  int contextXPos() const {
    return m_contextLabel.bounds().x - bounds().x;
  }

private:
  HeaderSplitter m_splitter1, m_splitter2;
  Label m_actionLabel;
  Label m_keyLabel;
  Label m_contextLabel;
};

class KeyItemBase : public ListItem {
public:
  KeyItemBase(const std::string& text)
    : ListItem(text) {
  }

protected:
  void onSizeHint(SizeHintEvent& ev) override {
    gfx::Size size = textSize();
    size.w = size.w + border().width();
    size.h = size.h + border().height() + 6*guiscale();
    ev.setSizeHint(size);
  }

};

class KeyItem : public KeyItemBase {

  // Used to avoid deleting the Add/Change/Del buttons on
  // kMouseLeaveMessage when a foreground window is popup on a signal
  // generated by those same buttons.
  struct LockButtons {
    KeyItem* keyItem;
    LockButtons(KeyItem* keyItem) : keyItem(keyItem) {
      keyItem->m_lockButtons = true;
    };
    ~LockButtons() {
      keyItem->m_lockButtons = false;
    };
  };

public:
  KeyItem(KeyboardShortcuts& keys,
          MenuKeys& menuKeys,
          const std::string& text,
          const KeyPtr& key,
          AppMenuItem* menuitem,
          const int level,
          HeaderItem* headerItem)
    : KeyItemBase(text)
    , m_keys(keys)
    , m_menuKeys(menuKeys)
    , m_key(key)
    , m_keyOrig(key ? new Key(*key): nullptr)
    , m_menuitem(menuitem)
    , m_level(level)
    , m_hotAccel(-1)
    , m_lockButtons(false)
    , m_headerItem(headerItem) {
    gfx::Border border = this->border();
    border.top(0);
    border.bottom(0);
    setBorder(border);
  }

  KeyPtr key() { return m_key; }
  AppMenuItem* menuitem() const { return m_menuitem; }

  std::string searchableText() const {
    if (m_menuitem) {
      Widget* w = m_menuitem;

      // If the menu has a submenu, this item cannot be triggered with a key
      // TODO make this possible: we should be able to open a menu with a key
      if (w->type() == kMenuItemWidget &&
          static_cast<MenuItem*>(w)->getSubmenu())
        return std::string();

      std::string result;
      while (w && w->type() == kMenuItemWidget) {
        if (!result.empty())
          result.insert(0, " > ");
        result.insert(0, w->text());

        w = w->parent();
        if (w && w->type() == kMenuWidget) {
          auto owner = static_cast<Menu*>(w)->getOwnerMenuItem();

          // Add the text of the menu (useful for the Palette Menu)
          if (!owner && !w->text().empty()) {
            result.insert(0, " > ");
            result.insert(0, w->text());
          }

          w = owner;
        }
        else {
          w = nullptr;
        }
      }
      return result;
    }
    else {
      return text();
    }
  }

private:

  void onChangeAccel(int index) {
    LockButtons lock(this);
    Accelerator origAccel = m_key->accels()[index];
    SelectAccelerator window(origAccel,
                             m_key->keycontext(),
                             m_keys);
    window.openWindowInForeground();

    if (window.isModified()) {
      m_key->disableAccel(origAccel, KeySource::UserDefined);
      if (!window.accel().isEmpty())
        m_key->add(window.accel(), KeySource::UserDefined, m_keys);
    }

    this->window()->layout();
  }

  void onDeleteAccel(int index) {
    LockButtons lock(this);
    // We need to create a copy of the accelerator because
    // Key::disableAccel() will modify the accels() collection itself.
    ui::Accelerator accel = m_key->accels()[index];

    if (ui::Alert::show(
          fmt::format(
            Strings::alerts_delete_shortcut(),
            accel.toString())) != 1)
      return;

    m_key->disableAccel(accel, KeySource::UserDefined);
    window()->layout();
  }

  void onAddAccel() {
    LockButtons lock(this);
    ui::Accelerator accel;
    SelectAccelerator window(accel,
                             m_key ? m_key->keycontext(): KeyContext::Any,
                             m_keys);
    window.openWindowInForeground();

    if ((window.isModified()) ||
        // We can assign a "None" accelerator to mouse wheel actions
        (m_key && m_key->type() == KeyType::WheelAction && window.isOK())) {
      if (!m_key) {
        ASSERT(m_menuitem);
        if (!m_menuitem)
          return;

        ASSERT(m_menuitem->getCommand());

        m_key = m_keys.command(
          m_menuitem->getCommandId().c_str(),
          m_menuitem->getParams());

        m_menuKeys[m_menuitem] = m_key;
      }

      m_key->add(window.accel(), KeySource::UserDefined, m_keys);
    }

    this->window()->layout();
  }

  void onSizeHint(SizeHintEvent& ev) override {
    KeyItemBase::onSizeHint(ev);
    gfx::Size size = ev.sizeHint();

    if (m_key && m_key->keycontext() != KeyContext::Any) {
      int w =
        m_headerItem->contextXPos() +
        Graphics::measureUITextLength(
          convertKeyContextToUserFriendlyString(m_key->keycontext()), font());
      size.w = std::max(size.w, w);
    }

    if (m_key && !m_key->accels().empty()) {
      size_t combos = m_key->accels().size();
      if (combos > 1)
        size.h *= combos;
    }

    ev.setSizeHint(size);
  }

  void onPaint(PaintEvent& ev) override {
    Graphics* g = ev.graphics();
    auto theme = SkinTheme::get(this);
    gfx::Rect bounds = clientBounds();
    gfx::Color fg, bg;

    if (isSelected()) {
      fg = theme->colors.listitemSelectedText();
      bg = theme->colors.listitemSelectedFace();
    }
    else {
      fg = theme->colors.listitemNormalText();
      bg = theme->colors.listitemNormalFace();
    }

    g->fillRect(bg, bounds);

    int y = bounds.y + 2*guiscale();
    const int th = textSize().h;
    // Position of the second and third columns
    const int keyXPos = bounds.x + m_headerItem->keyXPos();
    const int contextXPos = bounds.x + m_headerItem->contextXPos();

    bounds.shrink(border());
    {
      int x = bounds.x + m_level*16 * guiscale();
      IntersectClip clip(g, gfx::Rect(x, y, keyXPos - x, th));
      if (clip) {
        g->drawUIText(text(), fg, bg, gfx::Point(x, y), 0);
      }
    }

    if (m_key && !m_key->accels().empty()) {
      if (m_key->keycontext() != KeyContext::Any) {
        g->drawText(
          convertKeyContextToUserFriendlyString(m_key->keycontext()), fg, bg,
          gfx::Point(contextXPos, y));
      }

      const int dh = th + 4*guiscale();
      IntersectClip clip(g, gfx::Rect(keyXPos, y,
                                      contextXPos - keyXPos,
                                      dh * m_key->accels().size()));
      if (clip) {
        int i = 0;
        for (const Accelerator& accel : m_key->accels()) {
          if (i != m_hotAccel || !m_changeButton) {
            g->drawText(
              getAccelText(accel), fg, bg,
              gfx::Point(keyXPos, y));
          }
          y += dh;
          ++i;
        }
      }
    }
  }

  void onResize(ResizeEvent& ev) override {
    KeyItemBase::onResize(ev);
    destroyButtons();
  }

  bool onProcessMessage(Message* msg) override {
    switch (msg->type()) {

      case kMouseLeaveMessage: {
        destroyButtons();
        invalidate();
        break;
      }

      case kMouseMoveMessage: {
        if (!isEnabled())
          break;

        gfx::Rect bounds = this->bounds();
        MouseMessage* mouseMsg = static_cast<MouseMessage*>(msg);

        const Accelerators* accels = (m_key ? &m_key->accels() : NULL);
        int y = bounds.y;
        int dh = textSize().h + 4*guiscale();
        int maxi = (accels && accels->size() > 1 ? accels->size(): 1);

        auto theme = SkinTheme::get(this);

        for (int i=0; i<maxi; ++i, y += dh) {
          int w = Graphics::measureUITextLength(
            (accels && i < (int)accels->size() ? getAccelText((*accels)[i]).c_str(): ""),
            font());
          gfx::Rect itemBounds(bounds.x + m_headerItem->keyXPos(), y, w, dh);
          itemBounds = itemBounds.enlarge(
            gfx::Border(
              4*guiscale(), 0,
              6*guiscale(), 1*guiscale()));

          if (accels &&
              i < (int)accels->size() &&
              mouseMsg->position().y >= itemBounds.y &&
              mouseMsg->position().y < itemBounds.y+itemBounds.h) {
            if (m_hotAccel != i) {
              m_hotAccel = i;

              m_changeConn = obs::connection();
              m_changeButton.reset(new Button(""));
              m_changeConn = m_changeButton->Click.connect([this, i]{ onChangeAccel(i); });
              m_changeButton->setStyle(theme->styles.miniButton());
              addChild(m_changeButton.get());

              m_deleteConn = obs::connection();
              m_deleteButton.reset(new Button(""));
              m_deleteConn = m_deleteButton->Click.connect([this, i]{ onDeleteAccel(i); });
              m_deleteButton->setStyle(theme->styles.miniButton());
              addChild(m_deleteButton.get());

              m_changeButton->setBgColor(gfx::ColorNone);
              m_changeButton->setBounds(itemBounds);
              m_changeButton->setText(getAccelText((*accels)[i]));

              const char* label = "x";
              m_deleteButton->setBgColor(gfx::ColorNone);
              m_deleteButton->setBounds(
                gfx::Rect(
                  itemBounds.x + itemBounds.w + 2*guiscale(),
                  itemBounds.y,
                  Graphics::measureUITextLength(
                    label, font()) + 4*guiscale(),
                  itemBounds.h));
              m_deleteButton->setText(label);

              invalidate();
            }
          }

          if (i == 0 && !m_addButton &&
              (!m_menuitem || m_menuitem->getCommand())) {
            m_addConn = obs::connection();
            m_addButton.reset(new Button(""));
            m_addConn = m_addButton->Click.connect([this]{ onAddAccel(); });
            m_addButton->setStyle(theme->styles.miniButton());
            addChild(m_addButton.get());

            itemBounds.w = 8*guiscale() + Graphics::measureUITextLength("Add", font());
            itemBounds.x -= itemBounds.w + 2*guiscale();

            m_addButton->setBgColor(gfx::ColorNone);
            m_addButton->setBounds(itemBounds);
            m_addButton->setText(Strings::keyboard_shortcuts_add());

            invalidate();
          }
        }
        break;
      }
    }
    return KeyItemBase::onProcessMessage(msg);
  }

  void destroyButtons() {
    m_changeConn = obs::connection();
    m_deleteConn = obs::connection();
    m_addConn = obs::connection();

    if (!m_lockButtons) {
      m_changeButton.reset();
      m_deleteButton.reset();
      m_addButton.reset();
    }
    // Just hide the buttons
    else {
      if (m_changeButton) m_changeButton->setVisible(false);
      if (m_deleteButton) m_deleteButton->setVisible(false);
      if (m_addButton) m_addButton->setVisible(false);
    }

    m_hotAccel = -1;
  }

  std::string getAccelText(const Accelerator& accel) const {
    if (m_key && m_key->type() == KeyType::WheelAction &&
        accel.isEmpty()) {
      return Strings::keyboard_shortcuts_default_action();
    }
    else {
      return accel.toString();
    }
  }

  KeyboardShortcuts& m_keys;
  MenuKeys& m_menuKeys;
  KeyPtr m_key;
  KeyPtr m_keyOrig;
  AppMenuItem* m_menuitem;
  int m_level;
  ui::Accelerators m_newAccels;
  std::shared_ptr<ui::Button> m_changeButton;
  std::shared_ptr<ui::Button> m_deleteButton;
  std::shared_ptr<ui::Button> m_addButton;
  obs::scoped_connection m_changeConn;
  obs::scoped_connection m_deleteConn;
  obs::scoped_connection m_addConn;
  int m_hotAccel;
  bool m_lockButtons;
  HeaderItem* m_headerItem;
};

class KeyboardShortcutsWindow : public app::gen::KeyboardShortcuts {
  // TODO Merge with CanvasSizeWindow::Dir
  enum class Dir { NW, N, NE, W, C, E, SW, S, SE };

public:
  KeyboardShortcutsWindow(app::KeyboardShortcuts& keys,
                          MenuKeys& menuKeys,
                          const std::string& searchText,
                          int& curSection)
    : m_keys(keys)
    , m_menuKeys(menuKeys)
    , m_searchChange(false)
    , m_wasDefault(false)
    , m_curSection(curSection) {
    setAutoRemap(false);

    m_listBoxes.push_back(menus());
    m_listBoxes.push_back(commands());
    m_listBoxes.push_back(tools());
    m_listBoxes.push_back(actions());
    m_listBoxes.push_back(wheelActions());
    m_listBoxes.push_back(dragActions());

#ifdef __APPLE__ // Zoom sliding two fingers option only on macOS
    slideZoom()->setVisible(true);
#else
    slideZoom()->setVisible(false);
#endif

    wheelBehavior()->setSelectedItem(
      m_keys.hasMouseWheelCustomization() ? 1: 0);
    if (isDefaultWheelBehavior()) {
      m_keys.setDefaultMouseWheelKeys(wheelZoom()->isSelected());
      m_wasDefault = true;
    }
    m_keys.addMissingMouseWheelKeys();
    updateSlideZoomText();

    onWheelBehaviorChange();

    wheelBehavior()->ItemChange.connect([this]{ onWheelBehaviorChange(); });
    wheelZoom()->Click.connect([this]{ onWheelZoomChange(); });

    search()->Change.connect([this]{ onSearchChange(); });
    section()->Change.connect([this]{ onSectionChange(); });
    dragActions()->Change.connect([this]{ onDragActionsChange(); });
    dragAngle()->ItemChange.connect([this]{ onDragVectorChange(); });
    dragDistance()->Change.connect([this]{ onDragVectorChange(); });
    importButton()->Click.connect([this]{ onImport(); });
    exportButton()->Click.connect([this]{ onExport(); });
    resetButton()->Click.connect([this]{ onReset(); });

    fillAllLists();

    if (!searchText.empty()) {
      search()->setText(searchText);
      onSearchChange();
    }
  }

  ~KeyboardShortcutsWindow() {
    deleteAllKeyItems();
  }

  bool isDefaultWheelBehavior() {
    return (wheelBehavior()->selectedItem() == 0);
  }

private:

  void deleteAllKeyItems() {
    deleteList(searchList());
    deleteList(menus());
    deleteList(commands());
    deleteList(tools());
    deleteList(actions());
    deleteList(wheelActions());
    deleteList(dragActions());
  }

  void fillAllLists() {
    deleteAllKeyItems();

    // Fill each list box with the keyboard shortcuts...

    fillMenusList(menus(), AppMenus::instance()->getRootMenu(), 0);

    {
      // Create a pseudo-item for the palette menu
      KeyItemBase* listItem = new KeyItemBase(
        Strings::palette_popup_menu_title());
      menus()->addChild(listItem);
      fillMenusList(menus(), AppMenus::instance()->getPalettePopupMenu(), 1);
    }

    fillToolsList(tools(), App::instance()->toolBox());
    fillWheelActionsList();
    fillDragActionsList();

    for (const KeyPtr& key : m_keys) {
      if (key->type() == KeyType::Tool ||
          key->type() == KeyType::Quicktool ||
          key->type() == KeyType::WheelAction ||
          key->type() == KeyType::DragAction) {
        continue;
      }

      std::string text = key->triggerString();
      switch (key->keycontext()) {
        case KeyContext::SelectionTool:
        case KeyContext::TranslatingSelection:
        case KeyContext::ScalingSelection:
        case KeyContext::RotatingSelection:
        case KeyContext::MoveTool:
        case KeyContext::FreehandTool:
        case KeyContext::ShapeTool:
          text =
            convertKeyContextToUserFriendlyString(key->keycontext())
            + ": " + text;
          break;
      }
      KeyItem* keyItem = new KeyItem(m_keys, m_menuKeys, text, key,
                                     nullptr, 0, &m_headerItem);

      ListBox* listBox = nullptr;
      switch (key->type()) {
        case KeyType::Command:
          listBox = this->commands();
          break;
        case KeyType::Action:
          listBox = this->actions();
          break;
      }

      ASSERT(listBox);
      if (listBox)
        listBox->addChild(keyItem);
    }

    commands()->sortItems();
    tools()->sortItems();
    actions()->sortItems();

    section()->selectIndex(m_curSection);
    updateViews();
  }

  void deleteList(Widget* listbox) {
    if (m_headerItem.parent() == listbox)
      listbox->removeChild(&m_headerItem);

    while (auto item = listbox->lastChild()) {
      listbox->removeChild(item);
      delete item;
    }
  }

  void fillSearchList(const std::string& search) {
    deleteList(searchList());

    MatchWords match(search);

    int sectionIdx = 0;         // index 0 is menus
    for (auto listBox : m_listBoxes) {
      Separator* group = nullptr;

      for (auto item : listBox->children()) {
        if (KeyItem* keyItem = dynamic_cast<KeyItem*>(item)) {
          std::string itemText = keyItem->searchableText();
          if (!match(itemText))
            continue;

          if (!group) {
            group = new SeparatorInView(
              section()->children()[sectionIdx]->text(), HORIZONTAL);
            searchList()->addChild(group);
          }

          KeyItem* copyItem =
            new KeyItem(m_keys, m_menuKeys, itemText, keyItem->key(),
                        keyItem->menuitem(), 0, &m_headerItem);

          if (!item->isEnabled())
            copyItem->setEnabled(false);

          searchList()->addChild(copyItem);
        }
      }

      ++sectionIdx;
    }
  }

  void onWheelBehaviorChange() {
    const bool isDefault = isDefaultWheelBehavior();
    wheelActions()->setEnabled(!isDefault);
    wheelZoom()->setVisible(isDefault);

    if (isDefault) {
      m_keys.setDefaultMouseWheelKeys(wheelZoom()->isSelected());
      m_wasDefault = true;
    }
    else if (m_wasDefault) {
      m_wasDefault = false;
      for (KeyPtr& key : m_keys) {
        if (key->type() == KeyType::WheelAction)
          key->copyOriginalToUser();
      }
    }
    m_keys.addMissingMouseWheelKeys();
    updateSlideZoomText();

    fillWheelActionsList();
    updateViews();
  }

  void updateSlideZoomText() {
    slideZoom()->setText(
      isDefaultWheelBehavior() ?
      Strings::options_slide_zoom():
      Strings::keyboard_shortcuts_slide_as_wheel());
  }

  void fillWheelActionsList() {
    deleteList(wheelActions());
    for (const KeyPtr& key : m_keys) {
      if (key->type() == KeyType::WheelAction) {
        KeyItem* keyItem = new KeyItem(
          m_keys, m_menuKeys, key->triggerString(), key,
          nullptr, 0, &m_headerItem);
        wheelActions()->addChild(keyItem);
      }
    }
    wheelActions()->sortItems();
  }

  void fillDragActionsList() {
    deleteList(dragActions());
    for (const KeyPtr& key : m_keys) {
      if (key->type() == KeyType::DragAction) {
        KeyItem* keyItem = new KeyItem(
          m_keys, m_menuKeys, key->triggerString(), key,
          nullptr, 0, &m_headerItem);
        dragActions()->addChild(keyItem);
      }
    }
    dragActions()->sortItems();
  }

  void onWheelZoomChange() {
    const bool isDefault = isDefaultWheelBehavior();
    if (isDefault)
      onWheelBehaviorChange();
  }

  void onSearchChange() {
    base::ScopedValue flag(m_searchChange, true);
    std::string searchText = search()->text();

    if (searchText.empty())
      section()->selectIndex(m_curSection);
    else {
      fillSearchList(searchText);
      section()->selectChild(nullptr);
    }

    updateViews();
  }

  void onSectionChange() {
    if (m_searchChange)
      return;

    search()->setText("");
    updateViews();
  }

  void onDragActionsChange() {
    auto key = selectedDragActionKey();
    if (!key)
      return;

    int angle = 180 * key->dragVector().angle() / PI;

    ui::Widget* oldFocus = manager()->getFocus();
    dragAngle()->setSelectedItem((int)angleToDir(angle));
    if (oldFocus)
      oldFocus->requestFocus();

    dragDistance()->setValue(key->dragVector().magnitude());
  }

  void onDragVectorChange() {
    auto key = selectedDragActionKey();
    if (!key)
      return;

    auto v = key->dragVector();
    double a = dirToAngle((Dir)dragAngle()->selectedItem()).angle();
    double m = dragDistance()->getValue();
    v.x = m * std::cos(a);
    v.y = m * std::sin(a);
    if (std::fabs(v.x) < 0.00001) v.x = 0.0;
    if (std::fabs(v.y) < 0.00001) v.y = 0.0;
    key->setDragVector(v);
  }

  void updateViews() {
    int s = section()->getSelectedIndex();
    if (s >= 0)
      m_curSection = s;

    searchView()->setVisible(s < 0);
    menusView()->setVisible(s == 0);
    commandsView()->setVisible(s == 1);
    toolsView()->setVisible(s == 2);
    actionsView()->setVisible(s == 3);
    wheelSection()->setVisible(s == 4);
    dragSection()->setVisible(s == 5);

    if (m_headerItem.parent())
      m_headerItem.parent()->removeChild(&m_headerItem);
    if (s < 0)
      searchList()->insertChild(0, &m_headerItem);
    else
      m_listBoxes[s]->insertChild(0, &m_headerItem);

    listsPlaceholder()->layout();
  }

  void onImport() {
    base::paths exts = { KEYBOARD_FILENAME_EXTENSION };
    base::paths filename;
    if (!app::show_file_selector(
          Strings::keyboard_shortcuts_import_keyboard_sc(), "", exts,
          FileSelectorType::Open, filename))
      return;

    ASSERT(!filename.empty());

    m_keys.importFile(filename.front(), KeySource::UserDefined);

    fillAllLists();
  }

  void onExport() {
    base::paths exts = { KEYBOARD_FILENAME_EXTENSION };
    base::paths filename;

    if (!app::show_file_selector(
          Strings::keyboard_shortcuts_export_keyboard_sc(), "", exts,
          FileSelectorType::Save, filename))
      return;

    ASSERT(!filename.empty());

    m_keys.exportFile(filename.front());
  }

  void onReset() {
    if (ui::Alert::show(Strings::alerts_restore_all_shortcuts()) == 1) {
      m_keys.reset();
      if (!isDefaultWheelBehavior()) {
        wheelBehavior()->setSelectedItem(0);
        onWheelBehaviorChange();
      }
      listsPlaceholder()->layout();
    }
  }

  void fillMenusList(ListBox* listbox, Menu* menu, int level) {
    for (auto child : menu->children()) {
      if (AppMenuItem* menuItem = dynamic_cast<AppMenuItem*>(child)) {
        if (menuItem->isRecentFileItem())
          continue;

        KeyItem* keyItem = new KeyItem(
          m_keys, m_menuKeys,
          menuItem->text().c_str(),
          m_menuKeys[menuItem],
          menuItem, level,
          &m_headerItem);

        listbox->addChild(keyItem);

        if (menuItem->hasSubmenu())
          fillMenusList(listbox, menuItem->getSubmenu(), level+1);
      }
    }
  }

  void fillToolsList(ListBox* listbox, ToolBox* toolbox) {
    for (Tool* tool : *toolbox) {
      std::string text = tool->getText();

      KeyPtr key = m_keys.tool(tool);
      KeyItem* keyItem = new KeyItem(m_keys, m_menuKeys, text, key,
                                     nullptr, 0, &m_headerItem);
      listbox->addChild(keyItem);

      text += " (quick)";
      key = m_keys.quicktool(tool);
      keyItem = new KeyItem(m_keys, m_menuKeys, text, key,
                            nullptr, 0, &m_headerItem);
      listbox->addChild(keyItem);
    }
  }

  bool onProcessMessage(ui::Message* msg) override {
    switch (msg->type()) {
      case kOpenMessage:
        load_window_pos(this, "KeyboardShortcuts");
        invalidate();
        break;
      case kCloseMessage:
        save_window_pos(this, "KeyboardShortcuts");
        break;
    }
    return app::gen::KeyboardShortcuts::onProcessMessage(msg);
  }

  KeyPtr selectedDragActionKey() {
    auto item = dragActions()->getSelectedChild();
    if (KeyItem* keyItem = dynamic_cast<KeyItem*>(item)) {
      KeyPtr key = keyItem->key();
      if (key && key->type() == KeyType::DragAction)
        return key;
    }
    return nullptr;
  }

  Dir angleToDir(int angle) {
    if (angle >= -1*45/2 && angle < 1*45/2) return Dir::E;
    if (angle >=  1*45/2 && angle < 3*45/2) return Dir::NE;
    if (angle >=  3*45/2 && angle < 5*45/2) return Dir::N;
    if (angle >=  5*45/2 && angle < 7*45/2) return Dir::NW;
    if ((angle >=  7*45/2 && angle <= 180) ||
        (angle >=    -180 && angle <= -7*45/2)) return Dir::W;
    if (angle >  -7*45/2 && angle <= -5*45/2) return Dir::SW;
    if (angle >  -5*45/2 && angle <= -3*45/2) return Dir::S;
    if (angle >  -3*45/2 && angle <= -1*45/2) return Dir::SE;
    return Dir::C;
  }

  DragVector dirToAngle(Dir dir) {
    switch (dir) {
      case Dir::NW: return DragVector(-1, +1);
      case Dir::N:  return DragVector( 0, +1);
      case Dir::NE: return DragVector(+1, +1);
      case Dir::W:  return DragVector(-1,  0);
      case Dir::C:  return DragVector( 0,  0);
      case Dir::E:  return DragVector(+1,  0);
      case Dir::SW: return DragVector(-1, -1);
      case Dir::S:  return DragVector( 0, -1);
      case Dir::SE: return DragVector(+1, -1);
    }
    return DragVector();
  }

  app::KeyboardShortcuts& m_keys;
  MenuKeys& m_menuKeys;
  std::vector<ListBox*> m_listBoxes;
  bool m_searchChange;
  bool m_wasDefault;
  HeaderItem m_headerItem;
  int& m_curSection;
};

} // anonymous namespace

class KeyboardShortcutsCommand : public Command {
public:
  KeyboardShortcutsCommand();

protected:
  void onLoadParams(const Params& params) override;
  void onExecute(Context* context) override;

private:
  void fillMenusKeys(app::KeyboardShortcuts& keys,
                     MenuKeys& menuKeys, Menu* menu);

  std::string m_search;
};

KeyboardShortcutsCommand::KeyboardShortcutsCommand()
  : Command(CommandId::KeyboardShortcuts(), CmdUIOnlyFlag)
{
}

void KeyboardShortcutsCommand::onLoadParams(const Params& params)
{
  m_search = params.get("search");
}

void KeyboardShortcutsCommand::onExecute(Context* context)
{
  static int curSection = 0;

  app::KeyboardShortcuts* globalKeys = app::KeyboardShortcuts::instance();
  app::KeyboardShortcuts keys;
  keys.setKeys(*globalKeys, true);
  keys.addMissingKeysForCommands();

  MenuKeys menuKeys;
  fillMenusKeys(keys, menuKeys, AppMenus::instance()->getRootMenu());
  fillMenusKeys(keys, menuKeys, AppMenus::instance()->getPalettePopupMenu());

  // Here we copy the m_search field because
  // KeyboardShortcutsWindow::fillAllLists() modifies this same
  // KeyboardShortcutsCommand instance (so m_search will be "")
  // TODO Seeing this, we need a complete new way to handle UI commands execution
  std::string neededSearchCopy = m_search;
  KeyboardShortcutsWindow window(keys, menuKeys, neededSearchCopy, curSection);

  ui::Display* mainDisplay = Manager::getDefault()->display();
  ui::fit_bounds(mainDisplay, &window,
                 gfx::Rect(mainDisplay->size()),
                 [](const gfx::Rect& workarea,
                    gfx::Rect& bounds,
                    std::function<gfx::Rect(Widget*)> getWidgetBounds) {
                   gfx::Point center = bounds.center();
                   bounds.setSize(workarea.size()*3/4);
                   bounds.setOrigin(center - gfx::Point(bounds.size()/2));
                 });

  window.loadLayout();

  window.setVisible(true);
  window.openWindowInForeground();

  if (window.closer() == window.ok()) {
    globalKeys->setKeys(keys, false);
    for (const auto& p : menuKeys)
      p.first->setKey(p.second);

    // Save preferences in widgets that are bound to options automatically
    {
      Message msg(kSavePreferencesMessage);
      msg.setPropagateToChildren(true);
      window.sendMessage(&msg);
    }

    // Save keyboard shortcuts in configuration file
    {
      ResourceFinder rf;
      rf.includeUserDir("user." KEYBOARD_FILENAME_EXTENSION);
      std::string fn = rf.getFirstOrCreateDefault();
      globalKeys->exportFile(fn);
    }
  }

  AppMenus::instance()->syncNativeMenuItemKeyShortcuts();
}

void KeyboardShortcutsCommand::fillMenusKeys(app::KeyboardShortcuts& keys,
                                             MenuKeys& menuKeys,
                                             Menu* menu)
{
  for (auto child : menu->children()) {
    if (AppMenuItem* menuItem = dynamic_cast<AppMenuItem*>(child)) {
      if (menuItem->isRecentFileItem())
        continue;

      if (menuItem->getCommand()) {
        menuKeys[menuItem] =
          keys.command(menuItem->getCommandId().c_str(),
                       menuItem->getParams());
      }

      if (menuItem->hasSubmenu())
        fillMenusKeys(keys, menuKeys, menuItem->getSubmenu());
    }
  }
}

Command* CommandFactory::createKeyboardShortcutsCommand()
{
  return new KeyboardShortcutsCommand;
}

} // namespace app
