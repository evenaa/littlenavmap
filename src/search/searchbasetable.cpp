/*****************************************************************************
* Copyright 2015-2020 Alexander Barthel alex@littlenavmap.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "search/searchbasetable.h"

#include "atools.h"
#include "common/constants.h"
#include "common/mapcolors.h"
#include "common/unit.h"
#include "export/csvexporter.h"
#include "geo/calculations.h"
#include "gui/actiontool.h"
#include "gui/dialog.h"
#include "gui/itemviewzoomhandler.h"
#include "logbook/logdatacontroller.h"
#include "mapgui/mapmarkhandler.h"
#include "mapgui/mapwidget.h"
#include "navapp.h"
#include "options/optiondata.h"
#include "query/airportquery.h"
#include "query/mapquery.h"
#include "route/route.h"
#include "search/column.h"
#include "search/columnlist.h"
#include "search/searchcontroller.h"
#include "search/sqlcontroller.h"
#include "sql/sqlrecord.h"
#include "search/sqlmodel.h"
#include "ui_mainwindow.h"

#include <QTimer>
#include <QClipboard>
#include <QKeyEvent>
#include <QFileInfo>
#include <QStringBuilder>

using atools::gui::ActionTool;

/* When using distance search delay the update the table after 500 milliseconds */
const int DISTANCE_EDIT_UPDATE_TIMEOUT_MS = 500;

class ViewEventFilter :
  public QObject
{

public:
  ViewEventFilter(SearchBaseTable *parent)
    : QObject(parent), searchBase(parent)
  {
  }

private:
  virtual bool eventFilter(QObject *object, QEvent *event) override;

  SearchBaseTable *searchBase;
};

bool ViewEventFilter::eventFilter(QObject *object, QEvent *event)
{
  if(event->type() == QEvent::KeyPress)
  {
    QKeyEvent *pKeyEvent = dynamic_cast<QKeyEvent *>(event);
    if(pKeyEvent != nullptr)
    {
      switch(pKeyEvent->key())
      {
        case Qt::Key_Return:
          searchBase->showSelectedEntry();
          return true;
      }
    }
  }

  return QObject::eventFilter(object, event);
}

class SearchWidgetEventFilter :
  public QObject
{

public:
  SearchWidgetEventFilter(SearchBaseTable *parent)
    : QObject(parent), searchBase(parent)
  {
  }

private:
  virtual bool eventFilter(QObject *object, QEvent *event) override;

  SearchBaseTable *searchBase;
};

bool SearchWidgetEventFilter::eventFilter(QObject *object, QEvent *event)
{
  if(event->type() == QEvent::KeyPress)
  {
    QKeyEvent *pKeyEvent = dynamic_cast<QKeyEvent *>(event);
    if(pKeyEvent != nullptr)
    {
      switch(pKeyEvent->key())
      {
        case Qt::Key_Down:
          searchBase->activateView();
          return true;

        case Qt::Key_Return:
          searchBase->showFirstEntry();
          return true;
      }
    }
  }

  return QObject::eventFilter(object, event);
}

SearchBaseTable::SearchBaseTable(QMainWindow *parent, QTableView *tableView, ColumnList *columnList,
                                 si::TabSearchId tabWidgetIndex)
  : AbstractSearch(parent, tabWidgetIndex), columns(columnList), view(tableView)
{
  mapQuery = NavApp::getMapQueryGui();
  airportQuery = NavApp::getAirportQuerySim();

  zoomHandler = new atools::gui::ItemViewZoomHandler(view);
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
  connect(NavApp::navAppInstance(), &atools::gui::Application::fontChanged, this, &SearchBaseTable::fontChanged);
#endif

  Ui::MainWindow *ui = NavApp::getMainUi();

  // Avoid stealing of Ctrl-C from other default menus
  ui->actionSearchTableCopy->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchResetSearch->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchShowAll->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchShowInformation->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchShowApproaches->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchShowApproachCustom->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchShowDepartureCustom->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchShowOnMap->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchTableSelectNothing->setShortcutContext(Qt::WidgetWithChildrenShortcut);

  // TODO move to respective derived classes
  if(tabIndex == si::SEARCH_AIRPORT)
  {
    // Add departure/destination/alternate actions for airport search tab
    ui->actionSearchRouteAirportStart->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    ui->actionSearchRouteAirportDest->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    ui->actionSearchRouteAirportAlternate->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  }

  if(tabIndex == si::SEARCH_USER)
  {
    ui->actionSearchUserpointUndo->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    ui->actionSearchUserpointRedo->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  }

  if(tabIndex == si::SEARCH_LOG)
  {
    ui->actionSearchLogdataUndo->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    ui->actionSearchLogdataRedo->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  }

  // Need extra action connected to catch the default Ctrl-C in the table view
  connect(ui->actionSearchTableCopy, &QAction::triggered, this, &SearchBaseTable::tableCopyClipboard);

  // Actions that cover the whole dock window
  ui->dockWidgetSearch->addActions({ui->actionSearchResetSearch, ui->actionSearchShowAll});

  tableView->addActions({ui->actionSearchTableCopy, ui->actionSearchTableSelectNothing});

  // Add actions to this tab
  QWidget *currentTab = ui->tabWidgetSearch->widget(tabWidgetIndex);
  currentTab->addActions({ui->actionSearchShowInformation, ui->actionSearchShowApproaches, ui->actionSearchShowApproachCustom,
                          ui->actionSearchShowDepartureCustom, ui->actionSearchShowOnMap});

  if(tabIndex == si::SEARCH_AIRPORT)
    currentTab->addActions({ui->actionSearchRouteAirportStart, ui->actionSearchRouteAirportDest, ui->actionSearchRouteAirportAlternate});

  if(tabIndex == si::SEARCH_USER)
    currentTab->addActions({ui->actionSearchUserpointUndo, ui->actionSearchUserpointRedo});

  if(tabIndex == si::SEARCH_LOG)
    currentTab->addActions({ui->actionSearchLogdataUndo, ui->actionSearchLogdataRedo});

  // Update single shot timer
  updateTimer = new QTimer(this);
  updateTimer->setSingleShot(true);
  connect(updateTimer, &QTimer::timeout, this, &SearchBaseTable::editTimeout);
  connect(ui->actionSearchShowInformation, &QAction::triggered, this, &SearchBaseTable::showInformationTriggered);
  connect(ui->actionSearchShowApproaches, &QAction::triggered, this, &SearchBaseTable::showApproachesTriggered);
  connect(ui->actionSearchShowApproachCustom, &QAction::triggered, this, &SearchBaseTable::showApproachesCustomTriggered);
  connect(ui->actionSearchShowDepartureCustom, &QAction::triggered, this, &SearchBaseTable::showDeparturesCustomTriggered);
  connect(ui->actionSearchShowOnMap, &QAction::triggered, this, &SearchBaseTable::showOnMapTriggered);
  connect(ui->actionSearchTableSelectNothing, &QAction::triggered, this, &SearchBaseTable::nothingSelectedTriggered);

  if(tabIndex == si::SEARCH_AIRPORT)
  {
    // Connecti airport search actions directly since they can be called by shortcuts
    connect(ui->actionSearchRouteAirportStart, &QAction::triggered, this, &SearchBaseTable::routeSetDepartureAction);
    connect(ui->actionSearchRouteAirportDest, &QAction::triggered, this, &SearchBaseTable::routeSetDestinationAction);
    connect(ui->actionSearchRouteAirportAlternate, &QAction::triggered, this, &SearchBaseTable::routeAddAlternateAction);
  }

  // Load text size from options
  zoomHandler->zoomPercent(OptionData::instance().getGuiSearchTableTextSize());

  viewEventFilter = new ViewEventFilter(this);
  widgetEventFilter = new SearchWidgetEventFilter(this);
  view->installEventFilter(viewEventFilter);
}

SearchBaseTable::~SearchBaseTable()
{
  view->removeEventFilter(viewEventFilter);
  delete controller;
  delete csvExporter;
  delete updateTimer;
  delete zoomHandler;
  delete columns;
  delete viewEventFilter;
  delete widgetEventFilter;
}

void SearchBaseTable::fontChanged()
{
  qDebug() << Q_FUNC_INFO;

  zoomHandler->fontChanged();
  optionsChanged();
}

/* Copy the selected rows of the table view as CSV into clipboard */
void SearchBaseTable::tableCopyClipboard()
{
  if(view->isVisible())
  {
    qDebug() << Q_FUNC_INFO;
    QString csv;
    SqlController *c = controller;

    int exported = 0;
    if(controller->hasColumn("lonx") && controller->hasColumn("laty"))
    {
      // Full CSV export including coordinates and full rows
      exported = CsvExporter::selectionAsCsv(view, true /* header */, true /* rows */, csv,
                                             {tr("Longitude"), tr("Latitude")},
                                             [c](int index) -> QStringList
      {
        return {QLocale().toString(c->getRawData(index, "lonx").toFloat(), 'f', 8),
                QLocale().toString(c->getRawData(index, "laty").toFloat(), 'f', 8)};
      });
    }
    else
      // Copy only selected cells
      exported = CsvExporter::selectionAsCsv(view, false /* header */, false /* rows */, csv);

    if(!csv.isEmpty())
      QApplication::clipboard()->setText(csv);

    NavApp::setStatusMessage(QString(tr("Copied %1 entries to clipboard.")).arg(exported));
  }
}

void SearchBaseTable::initViewAndController(atools::sql::SqlDatabase *db)
{
  view->horizontalHeader()->setSectionsMovable(true);
  view->verticalHeader()->setSectionsMovable(false);
  view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

  delete controller;
  controller = new SqlController(db, columns, view);
  controller->prepareModel();

  csvExporter = new CsvExporter(mainWindow, controller);
}

void SearchBaseTable::showInSearch(const atools::sql::SqlRecord& record, bool ignoreQueryBuilder)
{
  controller->showInSearch(record, ignoreQueryBuilder);
}

void SearchBaseTable::optionsChanged()
{
  // Need to reset model for "treat empty icons special"
  preDatabaseLoad();
  postDatabaseLoad();

  // Adapt table view text size
  zoomHandler->zoomPercent(OptionData::instance().getGuiSearchTableTextSize());

  // Update the unit strings in the table header
  updateUnits();

  // Run searches again to reflect unit changes
  updateDistanceSearch();

  for(const Column *col : columns->getColumns())
  {
    if(col->getSpinBoxWidget() != nullptr)
      updateFromSpinBox(col->getSpinBoxWidget()->value(), col);

    if(col->getMaxSpinBoxWidget() != nullptr)
      updateFromMaxSpinBox(col->getMaxSpinBoxWidget()->value(), col);

    if(col->getMinSpinBoxWidget() != nullptr)
      updateFromMinSpinBox(col->getMinSpinBoxWidget()->value(), col);
  }
  view->update();
}

void SearchBaseTable::styleChanged()
{
  view->update();
}

void SearchBaseTable::updateTableSelection(bool noFollow)
{
  tableSelectionChangedInternal(noFollow);
}

void SearchBaseTable::searchMarkChanged(const atools::geo::Pos& mark)
{
  qDebug() << "new mark" << mark;
  if(columns->isDistanceCheckBoxChecked() && mark.isValid())
    updateDistanceSearch();
}

void SearchBaseTable::updateDistanceSearch()
{
  MapWidget *mapWidget = NavApp::getMapWidgetGui();

  if(columns->isDistanceCheckBoxChecked() &&
     mapWidget->getSearchMarkPos().isValid())
  {
    // Currently running distance search - update result
    QSpinBox *minDistanceWidget = columns->getMinDistanceWidget();
    QSpinBox *maxDistanceWidget = columns->getMaxDistanceWidget();
    QComboBox *distanceDirWidget = columns->getDistanceDirectionWidget();

    controller->filterByDistance(mapWidget->getSearchMarkPos(),
                                 static_cast<sqlmodeltypes::SearchDirection>(distanceDirWidget->currentIndex()),
                                 Unit::rev(minDistanceWidget->value(), Unit::distNmF),
                                 Unit::rev(maxDistanceWidget->value(), Unit::distNmF));

    controller->loadAllRowsForDistanceSearch();
  }
}

void SearchBaseTable::connectSearchWidgets()
{
  // Connect query builder callback to lambda ======================================
  if(columns->getQueryBuilder().isValid())
  {
    controller->setBuilder(columns->getQueryBuilder());

    QWidget *widget = columns->getQueryBuilder().getWidget();

    if(widget != nullptr)
    {
      QLineEdit *lineEdit = dynamic_cast<QLineEdit *>(widget);

      // Only line edit allowed for now
      if(lineEdit != nullptr)
      {
        connect(lineEdit, &QLineEdit::textChanged, this, [ = ](const QString& text)
        {
          Q_UNUSED(text)
          controller->filterByBuilder();
          updateButtonMenu();
          editStartTimer();
        });
      }
    }
  }

  // Connect all column assigned widgets to lambda ======================================
  for(const Column *col : columns->getColumns())
  {
    if(col->getLineEditWidget() != nullptr)
    {
      connect(col->getLineEditWidget(), &QLineEdit::textChanged, this, [ = ](const QString& text)
      {
        controller->filterByLineEdit(col, text);
        updateButtonMenu();
        editStartTimer();
      });
    }
    else if(col->getComboBoxWidget() != nullptr)
    {
      if(col->getComboBoxWidget()->isEditable())
      {
        // Treat editable combo boxes like line edits
        connect(col->getComboBoxWidget(), &QComboBox::editTextChanged, this, [ = ](const QString& text)
        {
          QComboBox *box = col->getComboBoxWidget();

          {
            QSignalBlocker blocker(box);

            // Reset index if entered word does not match
            QString txt = box->currentText();
            box->setCurrentIndex(box->findText(text, Qt::MatchExactly));
            box->setCurrentText(txt);
          }

          controller->filterByLineEdit(col, text);
          updateButtonMenu();
          editStartTimer();
        });
      }
      else
      {
        connect(col->getComboBoxWidget(), QOverload<int>::of(&QComboBox::currentIndexChanged), this, [ = ](int index)
        {
          controller->filterByComboBox(col, index, index == 0);
          updateButtonMenu();
          editStartTimer();
        });
      }
    }
    else if(col->getCheckBoxWidget() != nullptr)
    {
      connect(col->getCheckBoxWidget(), &QCheckBox::stateChanged, this, [ = ](int state)
      {
        controller->filterByCheckbox(col, state, col->getCheckBoxWidget()->isTristate());
        updateButtonMenu();
        editStartTimer();
      });
    }
    else if(col->getSpinBoxWidget() != nullptr)
    {
      connect(col->getSpinBoxWidget(), QOverload<int>::of(&QSpinBox::valueChanged), this, [ = ](int value)
      {
        updateFromSpinBox(value, col);
        updateButtonMenu();
        editStartTimer();
      });
    }
    else if(col->getMinSpinBoxWidget() != nullptr && col->getMaxSpinBoxWidget() != nullptr)
    {
      connect(col->getMinSpinBoxWidget(), QOverload<int>::of(&QSpinBox::valueChanged), this, [ = ](int value)
      {
        updateFromMinSpinBox(value, col);
        updateButtonMenu();
        editStartTimer();
      });

      connect(col->getMaxSpinBoxWidget(), QOverload<int>::of(&QSpinBox::valueChanged), this, [ = ](int value)
      {
        updateFromMaxSpinBox(value, col);
        updateButtonMenu();
        editStartTimer();
      });
    }
  }

  QSpinBox *minDistanceWidget = columns->getMinDistanceWidget();
  QSpinBox *maxDistanceWidget = columns->getMaxDistanceWidget();
  QComboBox *distanceDirWidget = columns->getDistanceDirectionWidget();
  QCheckBox *distanceCheckBox = columns->getDistanceCheckBox();

  if(minDistanceWidget != nullptr && maxDistanceWidget != nullptr &&
     distanceDirWidget != nullptr && distanceCheckBox != nullptr)
  {
    // If all distance widgets are present connect them
    connect(distanceCheckBox, &QCheckBox::stateChanged, this, &SearchBaseTable::distanceSearchStateChanged);

    connect(minDistanceWidget, QOverload<int>::of(&QSpinBox::valueChanged), this, [ = ](int value)
    {
      controller->filterByDistanceUpdate(
        static_cast<sqlmodeltypes::SearchDirection>(distanceDirWidget->currentIndex()),
        Unit::rev(value, Unit::distNmF),
        Unit::rev(maxDistanceWidget->value(), Unit::distNmF));

      maxDistanceWidget->setMinimum(value > 10 ? value : 10);
      updateButtonMenu();
      editStartTimer();
    });

    connect(maxDistanceWidget, QOverload<int>::of(&QSpinBox::valueChanged), this, [ = ](int value)
    {
      controller->filterByDistanceUpdate(
        static_cast<sqlmodeltypes::SearchDirection>(distanceDirWidget->currentIndex()),
        Unit::rev(minDistanceWidget->value(), Unit::distNmF),
        Unit::rev(value, Unit::distNmF));
      minDistanceWidget->setMaximum(value);
      updateButtonMenu();
      editStartTimer();
    });

    connect(distanceDirWidget, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [ = ](int index)
    {
      controller->filterByDistanceUpdate(static_cast<sqlmodeltypes::SearchDirection>(index),
                                         Unit::rev(minDistanceWidget->value(), Unit::distNmF),
                                         Unit::rev(maxDistanceWidget->value(), Unit::distNmF));
      updateButtonMenu();
      editStartTimer();
    });
  }
}

void SearchBaseTable::updateFromSpinBox(int value, const Column *col)
{
  if(col->getUnitConvert() != nullptr)
    // Convert widget units to internal units using the given function pointer
    controller->filterBySpinBox(
      col, atools::roundToInt(Unit::rev(value, col->getUnitConvert())));
  else
    controller->filterBySpinBox(col, value);
}

void SearchBaseTable::updateFromMinSpinBox(int value, const Column *col)
{
  float valMin = value, valMax = col->getMaxSpinBoxWidget()->value();

  if(col->getUnitConvert() != nullptr)
  {
    // Convert widget units to internal units using the given function pointer
    valMin = atools::roundToInt(Unit::rev(valMin, col->getUnitConvert()));
    valMax = atools::roundToInt(Unit::rev(valMax, col->getUnitConvert()));
  }

  controller->filterByMinMaxSpinBox(col, atools::roundToInt(valMin),
                                    atools::roundToInt(valMax));
  col->getMaxSpinBoxWidget()->setMinimum(value);
}

void SearchBaseTable::updateFromMaxSpinBox(int value, const Column *col)
{
  float valMin = col->getMinSpinBoxWidget()->value(), valMax = value;

  if(col->getUnitConvert() != nullptr)
  {
    // Convert widget units to internal units using the given function pointer
    valMin = atools::roundToInt(Unit::rev(valMin, col->getUnitConvert()));
    valMax = atools::roundToInt(Unit::rev(valMax, col->getUnitConvert()));
  }

  controller->filterByMinMaxSpinBox(col, atools::roundToInt(valMin), atools::roundToInt(valMax));
  col->getMinSpinBoxWidget()->setMaximum(value);
}

void SearchBaseTable::distanceSearchStateChanged(int state)
{
  distanceSearchChanged(state == Qt::Checked, true);
}

void SearchBaseTable::distanceSearchChanged(bool checked, bool changeViewState)
{
  MapWidget *mapWidget = NavApp::getMapWidgetGui();

  if((mapWidget->getSearchMarkPos().isNull() || !mapWidget->getSearchMarkPos().isValid()) && checked)
  {
    atools::gui::Dialog(mainWindow).showInfoMsgBox(lnm::ACTIONS_SHOW_SEARCH_CENTER_NULL,
                                                   tr("The search center is not set.\n"
                                                      "Right-click into the map and select\n"
                                                      "\"Set Center for Distance Search\"."),
                                                   tr("Do not &show this dialog again."));
  }

  QSpinBox *minDistanceWidget = columns->getMinDistanceWidget();
  QSpinBox *maxDistanceWidget = columns->getMaxDistanceWidget();
  QComboBox *distanceDirWidget = columns->getDistanceDirectionWidget();

  if(changeViewState)
    saveViewState(!checked);

  controller->filterByDistance(checked ? mapWidget->getSearchMarkPos() : atools::geo::Pos(),
                               static_cast<sqlmodeltypes::SearchDirection>(distanceDirWidget->currentIndex()),
                               Unit::rev(minDistanceWidget->value(), Unit::distNmF),
                               Unit::rev(maxDistanceWidget->value(), Unit::distNmF));

  minDistanceWidget->setEnabled(checked);
  maxDistanceWidget->setEnabled(checked);
  distanceDirWidget->setEnabled(checked);
  if(checked)
    controller->loadAllRowsForDistanceSearch();
  restoreViewState(checked);
  updateButtonMenu();
}

void SearchBaseTable::installEventFilterForWidget(QWidget *widget)
{
  widget->installEventFilter(widgetEventFilter);
}

/* Search criteria editing has started. Start or restart the timer for a
 * delayed update if distance search is used */
void SearchBaseTable::editStartTimer()
{
  if(controller->isDistanceSearch())
  {
    qDebug() << "editStarted";
    updateTimer->start(DISTANCE_EDIT_UPDATE_TIMEOUT_MS);
  }
}

/* Delayed update timeout. Update result if distance search is active */
void SearchBaseTable::editTimeout()
{
  qDebug() << "editTimeout";
  controller->loadAllRowsForDistanceSearch();
}

void SearchBaseTable::connectSearchSlots()
{
  connect(view, &QTableView::doubleClicked, this, &SearchBaseTable::doubleClick);
  connect(view, &QTableView::customContextMenuRequested, this, &SearchBaseTable::contextMenu);

  Ui::MainWindow *ui = NavApp::getMainUi();

  connect(ui->actionSearchShowAll, &QAction::triggered, this, &SearchBaseTable::loadAllRowsIntoView);
  connect(ui->actionSearchResetSearch, &QAction::triggered, this, &SearchBaseTable::resetSearch);

  reconnectSelectionModel();

  connect(controller->getSqlModel(), &SqlModel::modelReset, this, &SearchBaseTable::reconnectSelectionModel);
  connect(controller->getSqlModel(), &SqlModel::fetchedMore, this, &SearchBaseTable::fetchedMore);

  connect(ui->dockWidgetSearch, &QDockWidget::visibilityChanged, this, &SearchBaseTable::dockVisibilityChanged);
}

void SearchBaseTable::updateUnits()
{
  columns->updateUnits();
  controller->updateHeaderData();
}

void SearchBaseTable::clearSelection()
{
  view->clearSelection();
}

bool SearchBaseTable::hasSelection() const
{
  return view->selectionModel() == nullptr ? false : view->selectionModel()->hasSelection();
}

/* Connect selection model again after a SQL model reset */
void SearchBaseTable::reconnectSelectionModel()
{
  if(view->selectionModel() != nullptr)
  {
    void (SearchBaseTable::*selChangedPtr)(const QItemSelection& selected, const QItemSelection& deselected) =
      &SearchBaseTable::tableSelectionChanged;

    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, selChangedPtr);
  }
}

/* Slot for table selection changed */
void SearchBaseTable::tableSelectionChanged(const QItemSelection&, const QItemSelection&)
{
  tableSelectionChangedInternal(false /* follow selection */);
}

/* Update highlights if dock is hidden or shown (does not change for dock tab stacks) */
void SearchBaseTable::dockVisibilityChanged(bool)
{
  tableSelectionChangedInternal(true /* do not follow selection */);
}

void SearchBaseTable::fetchedMore()
{
  tableSelectionChangedInternal(true /* do not follow selection */);
}

void SearchBaseTable::tableSelectionChangedInternal(bool noFollow)
{
  QItemSelectionModel *sm = view->selectionModel();

  int selectedRows = 0;
  if(sm != nullptr && sm->hasSelection())
    selectedRows = sm->selectedRows().size();

  updatePushButtons();

  emit selectionChanged(this, selectedRows, controller->getVisibleRowCount(), controller->getTotalRowCount());

  // Follow selection =======================
  if(!noFollow)
  {
    if(sm != nullptr &&
       sm->currentIndex().isValid() &&
       sm->isSelected(sm->currentIndex()) &&
       followModeAction() != nullptr &&
       followModeAction()->isChecked())
      showRow(sm->currentIndex().row(), false /* show info */);
  }
}

void SearchBaseTable::preDatabaseLoad()
{
  saveViewState(controller->isDistanceSearch());
  controller->preDatabaseLoad();
}

void SearchBaseTable::postDatabaseLoad()
{
  controller->postDatabaseLoad();
  restoreViewState(controller->isDistanceSearch());
}

/* Reset view sort order, column width and column order back to default values */
void SearchBaseTable::resetView()
{
  if(NavApp::getSearchController()->getCurrentSearchTabId() == tabIndex)
  {
    controller->resetView();
    updatePushButtons();
    NavApp::setStatusMessage(tr("Table view reset to defaults."));
  }
}

void SearchBaseTable::refreshData(bool loadAll, bool keepSelection)
{
  controller->refreshData(loadAll, keepSelection);

  tableSelectionChangedInternal(true /* do not follow selection */);
}

void SearchBaseTable::refreshView()
{
  controller->refreshView();

  tableSelectionChangedInternal(true /* do not follow selection */);
}

int SearchBaseTable::getVisibleRowCount() const
{
  return controller->getVisibleRowCount();
}

int SearchBaseTable::getTotalRowCount() const
{
  return controller->getTotalRowCount();
}

int SearchBaseTable::getSelectedRowCount() const
{
  QItemSelectionModel *sm = view->selectionModel();

  int selectedRows = 0;
  if(sm != nullptr && sm->hasSelection())
    selectedRows = sm->selectedRows().size();

  return selectedRows;
}

QVector<int> SearchBaseTable::getSelectedIds() const
{
  QVector<int> retval;

  for(const QItemSelectionRange& rng : controller->getSelection())
  {
    for(int row = rng.top(); row <= rng.bottom(); ++row)
    {
      if(controller->hasRow(row))
        retval.append(controller->getRawData(row, columns->getIdColumnName()).toInt());
    }
  }
  return retval;
}

QVector<int> SearchBaseTable::getSelectedRows() const
{
  QVector<int> retval;

  for(const QItemSelectionRange& rng : controller->getSelection())
  {
    for(int row = rng.top(); row <= rng.bottom(); ++row)
    {
      if(controller->hasRow(row))
        retval.append(row);
    }
  }

  // Sort to get top of selection first in the list
  std::sort(retval.begin(), retval.end());
  return retval;
}

void SearchBaseTable::resetSearch()
{
  if(NavApp::getSearchController()->getCurrentSearchTabId() == tabIndex)
  {
    controller->resetSearch();
    updatePushButtons();
    NavApp::setStatusMessage(tr("Search filters cleared."));
  }
}

/* Loads all rows into the table view */
void SearchBaseTable::loadAllRowsIntoView()
{
  if(NavApp::getSearchController()->getCurrentSearchTabId() == tabIndex)
  {
    // bool allSelected = getSelectedRowCount() == getVisibleRowCount();

    // Clear selection since it can get invalid
    view->clearSelection();

    controller->loadAllRows();
    updatePushButtons();

    // if(allSelected)
    // view->selectAll();

    NavApp::setStatusMessage(tr("All entries read."));
  }
}

void SearchBaseTable::showFirstEntry()
{
  showRow(0, true /* show info */);
}

void SearchBaseTable::showSelectedEntry()
{
  QModelIndex idx = view->currentIndex();

  if(idx.isValid())
    showRow(idx.row(), true /* show info */);
}

void SearchBaseTable::activateView()
{
  // view->raise();
  // view->activateWindow();
  view->setFocus();
}

/* Double click into table view */
void SearchBaseTable::doubleClick(const QModelIndex& index)
{
  if(index.isValid())
    showRow(index.row(), true /* show info */);
}

void SearchBaseTable::showRow(int row, bool showInfo)
{
  qDebug() << Q_FUNC_INFO;

  // Show on information panel
  map::MapTypes navType = map::NONE;
  map::MapAirspaceSources airspaceSource = map::AIRSPACE_SRC_NONE;
  int id = -1;
  // get airport, VOR, NDB or waypoint id from model row
  getNavTypeAndId(row, navType, airspaceSource, id);

  if(id > 0 && navType != map::NONE)
  {
    // Check if the used table has bounding rectangle columns

    // Show on map
    if(columns->hasColumn("left_lonx") && columns->hasColumn("top_laty") &&
       columns->hasColumn("right_lonx") && columns->hasColumn("bottom_laty"))
    {
      // Rectangle at airports
      atools::geo::Pos topLeft(controller->getRawData(row, "left_lonx"),
                               controller->getRawData(row, "top_laty"));
      atools::geo::Pos bottomRight(controller->getRawData(row, "right_lonx"),
                                   controller->getRawData(row, "bottom_laty"));

      if(topLeft.isValid() && bottomRight.isValid())
        emit showRect(atools::geo::Rect(topLeft, bottomRight), true);
    }
    else if(columns->hasColumn("min_lonx") && columns->hasColumn("max_laty") &&
            columns->hasColumn("max_lonx") && columns->hasColumn("min_laty"))
    {
      // Different column names for airspaces and online centers
      atools::geo::Pos topLeft(controller->getRawData(row, "min_lonx"),
                               controller->getRawData(row, "max_laty"));
      atools::geo::Pos bottomRight(controller->getRawData(row, "max_lonx"),
                                   controller->getRawData(row, "min_laty"));

      if(topLeft.isValid() && bottomRight.isValid())
        emit showRect(atools::geo::Rect(topLeft, bottomRight), true);
    }
    else if(columns->hasColumn("departure_lonx") && columns->hasColumn("departure_laty") &&
            columns->hasColumn("destination_lonx") && columns->hasColumn("destination_laty"))
    {
      atools::geo::Pos depart(controller->getRawData(row, "departure_lonx"),
                              controller->getRawData(row, "departure_laty"));
      atools::geo::Pos dest(controller->getRawData(row, "destination_lonx"),
                            controller->getRawData(row, "destination_laty"));

      if(depart.isValid() && dest.isValid())
        emit showRect(atools::geo::boundingRect({depart, dest}), true);
      else if(depart.isValid())
        emit showPos(depart, 0.f, true);
      else if(dest.isValid())
        emit showPos(dest, 0.f, true);
    }
    else
    {
      atools::geo::Pos p(controller->getRawData(row, "lonx"), controller->getRawData(row, "laty"));
      if(p.isValid())
        emit showPos(p, 0.f, true);
    }

    if(showInfo)
    {
      map::MapResult result;
      mapQuery->getMapObjectById(result, navType, airspaceSource, id, false /* airport from nav database */);
      emit showInformation(result);
    }
  }
}

void SearchBaseTable::nothingSelectedTriggered()
{
  controller->selectNoRows();
}

/* Context menu in table view selected */
void SearchBaseTable::contextMenu(const QPoint& pos)
{
  qDebug() << Q_FUNC_INFO << "pos" << pos;

  static const int NAVAID_NAMES_ELIDE = 15;

  Ui::MainWindow *ui = NavApp::getMainUi();

  QPoint menuPos = QCursor::pos();
  // Use widget center if position is not inside widget
  if(!view->rect().contains(view->mapFromGlobal(QCursor::pos())))
    menuPos = view->mapToGlobal(view->rect().center());

  // Move menu position off the cursor to avoid accidental selection on touchpads
  menuPos += QPoint(3, 3);

  QString fieldData;

  // Save and restore action texts on return
  ActionTool actionTool({ui->actionLogdataAdd, ui->actionLogdataDelete, ui->actionLogdataEdit, ui->actionLogdataPerfLoad,
                         ui->actionLogdataRouteOpen, ui->actionMapHold, ui->actionMapNavaidRange, ui->actionMapRangeRings,
                         ui->actionMapTrafficPattern, ui->actionRouteAddPos, ui->actionRouteAppendPos, ui->actionSearchFilterExcluding,
                         ui->actionSearchFilterIncluding, ui->actionSearchLogdataOpenPerf, ui->actionSearchLogdataOpenPlan,
                         ui->actionSearchLogdataSaveGpxAs, ui->actionSearchLogdataSavePerfAs, ui->actionSearchLogdataSavePlanAs,
                         ui->actionSearchLogRouteAirportAlternate, ui->actionSearchLogRouteAirportDest,
                         ui->actionSearchLogRouteAirportStart, ui->actionSearchLogShowInformationAirport,
                         ui->actionSearchLogShowOnMapAirport, ui->actionSearchResetSearch, ui->actionSearchResetView,
                         ui->actionSearchRouteAirportAlternate, ui->actionSearchRouteAirportDest, ui->actionMapAirportMsa,
                         ui->actionSearchRouteAirportStart, ui->actionSearchSetMark, ui->actionSearchShowAll,
                         ui->actionSearchShowApproaches, ui->actionSearchShowApproachCustom, ui->actionSearchShowDepartureCustom,
                         ui->actionSearchShowInformation, ui->actionSearchShowOnMap, ui->actionSearchTableCopy,
                         ui->actionSearchTableSelectAll, ui->actionSearchTableSelectNothing, ui->actionUserdataAdd,
                         ui->actionUserdataDelete, ui->actionUserdataEdit});

  bool columnCanFilter = false, columnCanFilterBuilder = false;
  atools::geo::Pos position;

  // Get index from current, selection or cursor position ====================================================
  // Get field at cursor position
  QModelIndex index = controller->getModelIndexAt(pos);

  if(!index.isValid())
  {
    // Fall back to selction and get first field there
    QList<int> selectedRows = controller->getSelectedRows(false /* reverse */);
    if(!selectedRows.isEmpty())
      index = controller->getModelIndexFor(selectedRows.constFirst(), 0);
    else
      // Get current position
      index = controller->getCurrentIndex();

    if(!index.isValid() && controller->getTotalRowCount() > 0)
      // Simply get first entry in case of no selection and no current position
      index = controller->getModelIndexFor(0, 0);
  }

  // Get objects at index ========================================================
  map::MapTypes mapObjType = map::NONE;

  // Airport in airport search also used for airport below cursor in logbook
  map::MapAirport airport;
  map::MapLogbookEntry logEntry;
  atools::sql::SqlRecord logRecord;

  // True if airport below cursor in logbook and ident not empty
  bool logAirport = false;

  int id = -1;
  const Column *columnDescriptor = nullptr;
  QString objectText, navaidRangeText;
  map::MapResult result, msaResult;
  if(index.isValid())
  {
    columnDescriptor = columns->getColumn(index.column());
    Q_ASSERT(columnDescriptor != nullptr);
    columnCanFilterBuilder = columnDescriptor->isFilterByBuilder();
    columnCanFilter = columnDescriptor->isFilter() || columnCanFilterBuilder;

    if(columnCanFilter)
      // Disabled menu items don't need any content
      fieldData = atools::elideTextShort(controller->getFieldDataAt(index), 30);

    if(controller->hasColumn("lonx") && controller->hasColumn("laty"))
      // Get position to display range rings
      position = atools::geo::Pos(controller->getRawData(index.row(), "lonx"), controller->getRawData(index.row(), "laty"));

    // get airport, VOR, NDB or waypoint id from model row
    getNavTypeAndId(index.row(), mapObjType, id);

    map::MapAirspaceSource airspaceSrc = tabIndex == si::SEARCH_ONLINE_CENTER ? map::AIRSPACE_SRC_ONLINE : map::AIRSPACE_SRC_NONE;
    mapQuery->getMapObjectById(result, mapObjType, airspaceSrc, id, mapObjType != map::AIRPORT);

    std::initializer_list<map::MapTypes> msaTypeList = {map::AIRPORT, map::VOR, map::NDB, map::WAYPOINT};
    if(result.hasTypes(map::AIRPORT | map::VOR | map::NDB | map::WAYPOINT))
      NavApp::getMapQueryGui()->getMapObjectByIdent(msaResult, map::AIRPORT_MSA, result.getIdent(msaTypeList),
                                                    result.getRegion(msaTypeList), QString(), result.getPosition(msaTypeList));

    objectText = result.objectText(mapObjType, NAVAID_NAMES_ELIDE);
    if((result.hasVor() && result.vors.constFirst().range > 0) || (result.hasNdb() && result.ndbs.constFirst().range > 0))
      navaidRangeText = objectText;

    if(mapObjType == map::AIRPORT && result.hasAirports())
      airport = result.airports.constFirst();
    else if(mapObjType == map::LOGBOOK)
    {
      // Prepare airport for menu items in logbook
      logEntry = NavApp::getLogdataController()->getLogEntryById(id);
      logRecord = NavApp::getLogdataController()->getLogEntryRecordById(id);

      QString name = columnDescriptor->getColumnName();
      QList<map::MapAirport> airports;
      if(name == "destination_ident" || name == "destination_name")
        airports = airportQuery->getAirportsByOfficialIdent(logEntry.destinationIdent, &logEntry.destinationPos);
      else if(name == "departure_ident" || name == "departure_name")
        airports = airportQuery->getAirportsByOfficialIdent(logEntry.departureIdent, &logEntry.departurePos);

      if(!airports.isEmpty())
      {
        airport = airports.constFirst();
        logAirport = true;
      }
      else
        qWarning() << Q_FUNC_INFO << "No airport found";
    }
  }
  else
    qDebug() << "Invalid index at" << pos;

  // Add data to menu item text ======================================================================
  QString filterText = fieldData.isEmpty() ? QString() : tr("\"%1\"").arg(fieldData);

  ui->actionSearchFilterIncluding->setEnabled(!fieldData.isEmpty() && index.isValid() && columnCanFilter);
  ActionTool::setText(ui->actionSearchFilterIncluding, filterText);

  if(!columnCanFilterBuilder)
    ui->actionSearchFilterExcluding->setEnabled(!fieldData.isEmpty() && index.isValid() && columnCanFilter);
  else
    ui->actionSearchFilterExcluding->setEnabled(false);
  ActionTool::setText(ui->actionSearchFilterExcluding, filterText);

  int range = controller->hasColumn("range") ? controller->getRawData(index.row(), "range").toInt() : 0;
  ui->actionMapNavaidRange->setEnabled(range > 0 && (mapObjType == map::VOR || mapObjType == map::NDB));

  ui->actionRouteAddPos->setEnabled(mapObjType == map::VOR || mapObjType == map::NDB || mapObjType == map::WAYPOINT ||
                                    mapObjType == map::AIRPORT || mapObjType == map::USERPOINT);
  ui->actionRouteAppendPos->setEnabled(mapObjType == map::VOR || mapObjType == map::NDB || mapObjType == map::WAYPOINT ||
                                       mapObjType == map::AIRPORT || mapObjType == map::USERPOINT);

  // Airport actions ==============================================================
  const Route& route = NavApp::getRouteConst();
  bool disableDeparture = false, disableDestination = false, disableAlternate = false;
  QString departureSuffix = proc::procedureTextSuffixDeparture(route, airport, disableDeparture);
  QString destinationSuffix = proc::procedureTextSuffixDestination(route, airport, disableDestination);
  QString alternateSuffix = proc::procedureTextSuffixAlternate(route, airport, disableAlternate);

  // Airport search
  ui->actionSearchRouteAirportStart->setDisabled(disableDeparture);
  ui->actionSearchRouteAirportDest->setDisabled(disableDestination);
  ui->actionSearchRouteAirportAlternate->setDisabled(disableAlternate);

  // Logbook airport sub-menu
  ui->actionSearchLogRouteAirportStart->setDisabled(disableDeparture);
  ui->actionSearchLogRouteAirportDest->setDisabled(disableDestination);
  ui->actionSearchLogRouteAirportAlternate->setDisabled(disableAlternate);

  ui->actionSearchLogShowOnMapAirport->setEnabled(airport.isValid());
  ui->actionSearchLogShowInformationAirport->setEnabled(airport.isValid());

  // Pattern and hold ============================================================
  ui->actionMapTrafficPattern->setEnabled(mapObjType == map::AIRPORT && !airport.noRunways());
  ui->actionMapHold->setEnabled(mapObjType == map::VOR || mapObjType == map::NDB || mapObjType == map::WAYPOINT ||
                                mapObjType == map::USERPOINT || mapObjType == map::AIRPORT);

  // Airport procedure actions ==============================================================
  ui->actionSearchShowApproaches->setEnabled(false);
  ui->actionSearchShowApproachCustom->setEnabled(false);
  ui->actionSearchShowDepartureCustom->setEnabled(false);

  if(mapObjType == map::AIRPORT && airport.isValid())
  {
    bool departureFilter, arrivalFilter, hasDeparture, hasAnyArrival, airportDeparture, airportDestination,
         airportRoundTrip;

    route.getAirportProcedureFlags(airport, -1, departureFilter, arrivalFilter, hasDeparture,
                                   hasAnyArrival, airportDeparture, airportDestination, airportRoundTrip);

    if(hasAnyArrival || hasDeparture)
    {
      if(airportDeparture && !airportRoundTrip)
      {
        if(hasDeparture)
        {
          ui->actionSearchShowApproaches->setText(tr("Show Departure &Procedures for %1"));
          ActionTool::setText(ui->actionSearchShowApproaches, true, objectText);
        }
        else
          ui->actionSearchShowApproaches->setText(tr("Show procedures (no departure procedure)"));
      }
      else if(airportDestination && !airportRoundTrip)
      {
        if(hasAnyArrival)
        {
          ui->actionSearchShowApproaches->setText(tr("Show Arrival &Procedures for %1"));
          ActionTool::setText(ui->actionSearchShowApproaches, true, objectText);
        }
        else
          ui->actionSearchShowApproaches->setText(tr("Show procedures (no arrival procedure)"));
      }
      else
        ui->actionSearchShowApproaches->setEnabled(true);
    }
    else
      ui->actionSearchShowApproaches->setText(tr("Show Procedures (no procedure)"));

    ui->actionSearchShowApproachCustom->setEnabled(true);
    if(airportDestination)
      ui->actionSearchShowApproachCustom->setText(tr("Select &Destination Runway for %1 ..."));
    else
      ui->actionSearchShowApproachCustom->setText(tr("Select &Runway and use %1 as Destination ..."));

    ui->actionSearchShowDepartureCustom->setEnabled(true);
    if(airportDeparture)
      ui->actionSearchShowDepartureCustom->setText(tr("Select &Departure Runway for %1 ..."));
    else
      ui->actionSearchShowDepartureCustom->setText(tr("Select Runway and use %1 as &Departure ..."));
  }
  else
    ui->actionSearchShowApproaches->setText(tr("Show &Procedures"));

  if(airport.noRunways())
  {
    ActionTool::setText(ui->actionSearchShowApproachCustom, false, QString(), tr(" (no runway)"));
    ActionTool::setText(ui->actionSearchShowDepartureCustom, false, QString(), tr(" (no runway)"));
  }
  else
  {
    ActionTool::setText(ui->actionSearchShowApproachCustom, true, objectText);
    ActionTool::setText(ui->actionSearchShowDepartureCustom, true, objectText);
  }

  if(tabIndex == si::SEARCH_LOG)
  {
    QString airportText;
    if(airport.isValid())
      airportText = map::airportTextShort(airport, NAVAID_NAMES_ELIDE);
    else if(logAirport)
      airportText = tr("(Airport not found)");

    ui->actionSearchLogRouteAirportStart->setText(ui->actionSearchLogRouteAirportStart->text().arg(airportText) + departureSuffix);
    ui->actionSearchLogRouteAirportDest->setText(ui->actionSearchLogRouteAirportDest->text().arg(airportText) + destinationSuffix);
    ui->actionSearchLogRouteAirportAlternate->setText(ui->actionSearchLogRouteAirportAlternate->text().arg(airportText) + alternateSuffix);

    ui->actionSearchLogShowInformationAirport->setText(ui->actionSearchLogShowInformationAirport->text().arg(airportText));
    ui->actionSearchLogShowOnMapAirport->setText(ui->actionSearchLogShowOnMapAirport->text().arg(airportText));
  }

  ui->actionSearchTableCopy->setEnabled(index.isValid());
  ui->actionSearchTableSelectAll->setEnabled(controller->getTotalRowCount() > 0);
  ui->actionSearchTableSelectNothing->setEnabled(
    controller->getTotalRowCount() > 0 && (view->selectionModel() == nullptr ? false : view->selectionModel()->hasSelection()));

  // Add marks ==============================================================================
  // Update texts to give user a hint for hidden user features in the disabled menu items =====================

  ui->actionMapRangeRings->setEnabled(index.isValid() && position.isValid());
  ui->actionSearchSetMark->setEnabled(index.isValid() && position.isValid());

  ActionTool::setText(ui->actionMapRangeRings, result.hasTypes(map::AIRPORT | map::VOR | map::NDB | map::WAYPOINT | map::USERPOINT),
                      navaidRangeText);

  ActionTool::setText(ui->actionMapHold, result.hasTypes(map::AIRPORT | map::VOR | map::NDB | map::WAYPOINT | map::USERPOINT),
                      objectText);

  ui->actionMapAirportMsa->setEnabled(msaResult.hasAirportMsa());

  if(airport.isValid())
    ActionTool::setText(ui->actionMapTrafficPattern, !airport.noRunways(), objectText, tr(" (no runway)"));
  else
    ActionTool::setText(ui->actionMapTrafficPattern, false);

  ui->actionSearchShowInformation->setEnabled(mapObjType != map::NONE);
  ui->actionSearchShowOnMap->setEnabled(mapObjType != map::NONE);

  // General texts ==============================================================
  ActionTool::setText(ui->actionMapNavaidRange, navaidRangeText);

  // Airport search
  ui->actionSearchRouteAirportStart->setText(ui->actionSearchRouteAirportStart->text().arg(objectText) % departureSuffix);
  ui->actionSearchRouteAirportDest->setText(ui->actionSearchRouteAirportDest->text().arg(objectText) % destinationSuffix);
  ui->actionSearchRouteAirportAlternate->setText(ui->actionSearchRouteAirportAlternate->text().arg(objectText) % alternateSuffix);

  // Build the menu depending on tab =========================================================================

  // Replace any left over placeholders
  actionTool.finishTexts(objectText);

  int selectedRows = getSelectedRowCount();
  QMenu menu;
  menu.setToolTipsVisible(NavApp::isMenuToolTipsVisible());
  if(atools::contains(tabIndex,
                      {si::SEARCH_AIRPORT, si::SEARCH_NAV, si::SEARCH_USER, si::SEARCH_LOG, si::SEARCH_ONLINE_CENTER,
                       si::SEARCH_ONLINE_CLIENT}))
  {
    menu.addAction(ui->actionSearchShowInformation);
    menu.addAction(ui->actionSearchShowOnMap);
    menu.addSeparator();
  }

  if(tabIndex == si::SEARCH_USER)
  {
    menu.addAction(ui->actionSearchUserpointUndo);
    menu.addAction(ui->actionSearchUserpointRedo);
    menu.addSeparator();
  }

  if(tabIndex == si::SEARCH_LOG)
  {
    menu.addAction(ui->actionSearchLogdataUndo);
    menu.addAction(ui->actionSearchLogdataRedo);
    menu.addSeparator();
  }

  // Add extra menu items in the user defined waypoint table - these are already connected
  if(tabIndex == si::SEARCH_USER)
  {
    if(selectedRows > 1)
    {
      ui->actionUserdataEdit->setText(tr("&Edit Userpoints ..."));
      ui->actionUserdataDelete->setText(tr("&Delete Userpoints"));
    }
    else
    {
      ui->actionUserdataEdit->setText(tr("&Edit Userpoint %1 ...").arg(objectText));
      ui->actionUserdataDelete->setText(tr("&Delete Userpoint %1").arg(objectText));
    }

    menu.addAction(ui->actionUserdataAdd);
    menu.addAction(ui->actionUserdataEdit);
    menu.addAction(ui->actionUserdataDelete);
    menu.addSeparator();
  } // if(tabIndex == si::SEARCH_USER)

  if(atools::contains(tabIndex, {si::SEARCH_AIRPORT}))
  {
    menu.addAction(ui->actionSearchRouteAirportStart);
    menu.addAction(ui->actionSearchRouteAirportDest);
    menu.addAction(ui->actionSearchRouteAirportAlternate);
    menu.addSeparator();

    menu.addAction(ui->actionSearchShowApproaches);
    menu.addAction(ui->actionSearchShowDepartureCustom);
    menu.addAction(ui->actionSearchShowApproachCustom);
    menu.addSeparator();
  }

  if(atools::contains(tabIndex, {si::SEARCH_AIRPORT, si::SEARCH_NAV, si::SEARCH_USER}))
  {
    menu.addAction(ui->actionRouteAddPos);
    menu.addAction(ui->actionRouteAppendPos);
    menu.addSeparator();
  }

  if(atools::contains(tabIndex, {si::SEARCH_AIRPORT, si::SEARCH_NAV, si::SEARCH_USER, si::SEARCH_ONLINE_CENTER,
                                 si::SEARCH_ONLINE_CLIENT}))
  {
    menu.addAction(ui->actionMapRangeRings);
    if(atools::contains(tabIndex, {si::SEARCH_NAV}))
      menu.addAction(ui->actionMapNavaidRange);
    menu.addSeparator();
  }

  if(atools::contains(tabIndex, {si::SEARCH_AIRPORT, si::SEARCH_NAV, si::SEARCH_USER}))
  {
    if(atools::contains(tabIndex, {si::SEARCH_AIRPORT}))
      menu.addAction(ui->actionMapTrafficPattern);

    menu.addAction(ui->actionMapHold);

    if(atools::contains(tabIndex, {si::SEARCH_AIRPORT, si::SEARCH_NAV}))
      menu.addAction(ui->actionMapAirportMsa);
    menu.addSeparator();
  }

  if(atools::contains(tabIndex, {si::SEARCH_LOG}))
  {
    menu.addAction(ui->actionLogdataAdd);
    menu.addAction(ui->actionLogdataEdit);
    menu.addAction(ui->actionLogdataDelete);
    menu.addSeparator();

    // Logbook airport menu if clicked on departure or destination =======================
    // menu takes ownership
    QMenu *airportSub = menu.addMenu(tr("&Airport"));
    airportSub->setToolTipsVisible(NavApp::isMenuToolTipsVisible());
    airportSub->addAction(ui->actionSearchLogShowInformationAirport);
    airportSub->addAction(ui->actionSearchLogShowOnMapAirport);
    airportSub->addSeparator();
    airportSub->addAction(ui->actionSearchLogRouteAirportStart);
    airportSub->addAction(ui->actionSearchLogRouteAirportDest);
    airportSub->addAction(ui->actionSearchLogRouteAirportAlternate);
    menu.addSeparator();

    // menu takes ownership of sub
    QMenu *filesSub = menu.addMenu(tr("&Files"));
    filesSub->setToolTipsVisible(NavApp::isMenuToolTipsVisible());
    filesSub->addAction(ui->actionLogdataRouteOpen);
    filesSub->addAction(ui->actionLogdataPerfLoad);
    filesSub->addSeparator();
    filesSub->addAction(ui->actionSearchLogdataOpenPlan);
    filesSub->addAction(ui->actionSearchLogdataSavePlanAs);
    filesSub->addSeparator();
    filesSub->addAction(ui->actionSearchLogdataOpenPerf);
    filesSub->addAction(ui->actionSearchLogdataSavePerfAs);
    filesSub->addSeparator();
    filesSub->addAction(ui->actionSearchLogdataSaveGpxAs);
    menu.addSeparator();
  }

  if(tabIndex == si::SEARCH_LOG)
  {
    if(selectedRows > 1)
    {
      ui->actionLogdataEdit->setText(tr("&Edit Logbook Entries ..."));
      ui->actionLogdataDelete->setText(tr("&Delete Logbook Entries"));
    }
    else
    {
      ui->actionLogdataEdit->setText(tr("&Edit Logbook Entry %1 ...").arg(objectText));
      ui->actionLogdataDelete->setText(tr("&Delete Logbook Entry %1").arg(objectText));
    }

    // Logbook open or save attached files sub menu =====================================
    bool hasRoute = NavApp::getLogdataController()->hasRouteAttached(logEntry.id);
    ui->actionSearchLogdataOpenPlan->setEnabled(hasRoute);
    ui->actionSearchLogdataSavePlanAs->setEnabled(hasRoute);

    if(!hasRoute)
    {
      ui->actionSearchLogdataOpenPlan->setText(ui->actionSearchLogdataOpenPlan->text() + tr(" (no attachment)"));
      ui->actionSearchLogdataSavePlanAs->setText(ui->actionSearchLogdataSavePlanAs->text() + tr(" (no attachment)"));
    }

    bool perf = NavApp::getLogdataController()->hasPerfAttached(logEntry.id);
    ui->actionSearchLogdataOpenPerf->setEnabled(perf);
    ui->actionSearchLogdataSavePerfAs->setEnabled(perf);

    if(!perf)
    {
      ui->actionSearchLogdataOpenPerf->setText(ui->actionSearchLogdataOpenPerf->text() + tr(" (no attachment)"));
      ui->actionSearchLogdataSavePerfAs->setText(ui->actionSearchLogdataSavePerfAs->text() + tr(" (no attachment)"));
    }

    bool gpx = NavApp::getLogdataController()->hasTrackAttached(logEntry.id);
    ui->actionSearchLogdataSaveGpxAs->setEnabled(gpx);
    if(!gpx)
      ui->actionSearchLogdataSaveGpxAs->setText(ui->actionSearchLogdataSaveGpxAs->text() + tr(" (no attachment)"));

    // Logbook open referenced files menu =======================
    ui->actionLogdataRouteOpen->setEnabled(false);
    if(!logEntry.routeFile.isEmpty())
    {
      if(QFileInfo::exists(logEntry.routeFile))
      {
        ui->actionLogdataRouteOpen->setEnabled(true);
        ui->actionLogdataRouteOpen->setText(ui->actionLogdataRouteOpen->text().
                                            arg(atools::elideTextShort(QFileInfo(logEntry.routeFile).fileName(),
                                                                       NAVAID_NAMES_ELIDE)));
      }
      else
        ui->actionLogdataRouteOpen->setText(ui->actionLogdataRouteOpen->text().arg(tr("(file not found)")));
    }
    else
      ui->actionLogdataRouteOpen->setText(ui->actionLogdataRouteOpen->text().arg(QString()));

    ui->actionLogdataPerfLoad->setEnabled(false);
    if(!logEntry.perfFile.isEmpty())
    {
      if(QFileInfo::exists(logEntry.perfFile))
      {
        ui->actionLogdataPerfLoad->setEnabled(true);
        ui->actionLogdataPerfLoad->setText(ui->actionLogdataPerfLoad->text().
                                           arg(atools::elideTextShort(QFileInfo(logEntry.perfFile).fileName(),
                                                                      NAVAID_NAMES_ELIDE)));
      }
      else
        ui->actionLogdataPerfLoad->setText(ui->actionLogdataPerfLoad->text().arg(tr("(file not found)")));
    }
    else
      ui->actionLogdataPerfLoad->setText(ui->actionLogdataPerfLoad->text().arg(QString()));
  } // if(tabIndex == si::SEARCH_LOG)

  if(atools::contains(tabIndex,
                      {si::SEARCH_AIRPORT, si::SEARCH_NAV, si::SEARCH_USER, si::SEARCH_LOG, si::SEARCH_ONLINE_CENTER,
                       si::SEARCH_ONLINE_CLIENT}))
  {
    if(atools::contains(tabIndex, {si::SEARCH_LOG}))
    {
      // Logbook display options menu =======================
      QMenu *sub = menu.addMenu(tr("&View Options"));
      sub->setToolTipsVisible(NavApp::isMenuToolTipsVisible());
      sub->addAction(ui->actionSearchLogdataShowDirect);
      sub->addAction(ui->actionSearchLogdataShowRoute);
      sub->addAction(ui->actionSearchLogdataShowTrack);
      menu.addSeparator();
    }

    menu.addAction(ui->actionSearchFilterIncluding);
    menu.addAction(ui->actionSearchFilterExcluding);
    menu.addSeparator();

    menu.addAction(ui->actionSearchResetSearch);
    menu.addAction(ui->actionSearchShowAll);
    menu.addSeparator();
  }

  if(atools::contains(tabIndex,
                      {si::SEARCH_AIRPORT, si::SEARCH_NAV, si::SEARCH_USER, si::SEARCH_LOG, si::SEARCH_ONLINE_CENTER,
                       si::SEARCH_ONLINE_CLIENT}))
  {
    menu.addAction(followModeAction());
    menu.addSeparator();
  }

  menu.addAction(ui->actionSearchTableCopy);
  menu.addAction(ui->actionSearchTableSelectAll);
  menu.addAction(ui->actionSearchTableSelectNothing);
  menu.addSeparator();

  menu.addAction(ui->actionSearchResetView);
  menu.addSeparator();

  if(atools::contains(tabIndex,
                      {si::SEARCH_AIRPORT, si::SEARCH_NAV, si::SEARCH_USER, si::SEARCH_ONLINE_CENTER,
                       si::SEARCH_ONLINE_CLIENT}))
    menu.addAction(ui->actionSearchSetMark);

  QAction *action = menu.exec(menuPos);

  if(action != nullptr)
    qDebug() << Q_FUNC_INFO << "selected" << action->text();
  else
    qDebug() << Q_FUNC_INFO << "no action selected";

  if(action != nullptr)
  {
    MapWidget *mapWidget = NavApp::getMapWidgetGui();

    // A menu item was selected
    // Other actions with shortcuts are connected directly to methods/signals
    if(action == ui->actionSearchResetView)
      resetView();
    else if(action == ui->actionSearchTableCopy)
      tableCopyClipboard();
    else if(action == ui->actionSearchFilterIncluding)
      controller->filterIncluding(index, columnDescriptor == nullptr ? false : columnDescriptor->isFilterByBuilder());
    else if(action == ui->actionSearchFilterExcluding)
      controller->filterExcluding(index, columnDescriptor == nullptr ? false : columnDescriptor->isFilterByBuilder());
    else if(action == ui->actionSearchTableSelectAll)
      controller->selectAllRows();
    else if(action == ui->actionSearchTableSelectNothing)
      controller->selectNoRows();
    else if(action == ui->actionSearchSetMark)
      emit changeSearchMark(position);
    else if(action == ui->actionMapRangeRings)
      mapWidget->addRangeMark(position, true /* showDialog */);
    else if(action == ui->actionMapTrafficPattern)
      mapWidget->addPatternMark(airport);
    else if(action == ui->actionMapHold)
      mapWidget->addHold(result, atools::geo::EMPTY_POS);
    else if(action == ui->actionMapNavaidRange)
    {
      QString freqChannelStr;
      if(mapObjType == map::VOR)
      {
        int frequency = controller->getRawData(index.row(), "frequency").toInt();
        if(frequency > 0)
        {
          // Use frequency for VOR and VORTAC
          frequency /= 10;
          freqChannelStr = QString::number(frequency);
        }
        else
          // Use channel for TACAN
          freqChannelStr = controller->getRawData(index.row(), "channel").toString();
      }
      else if(mapObjType == map::NDB)
        freqChannelStr = controller->getRawData(index.row(), "frequency").toString();

      mapWidget->addNavRangeMark(position, mapObjType,
                                 controller->getRawData(index.row(), "ident").toString(),
                                 freqChannelStr, controller->getRawData(index.row(), "range").toInt());
    }
    // else if(action == ui->actionMapHideRangeRings)
    // NavApp::getMapWidget()->clearRangeRingsAndDistanceMarkers(); // Connected directly
    else if(action == ui->actionMapAirportMsa)
      emit addAirportMsa(msaResult.airportMsa.value(0));
    else if(action == ui->actionRouteAddPos)
      emit routeAdd(id, atools::geo::EMPTY_POS, mapObjType, -1);
    else if(action == ui->actionRouteAppendPos)
      emit routeAdd(id, atools::geo::EMPTY_POS, mapObjType, map::INVALID_INDEX_VALUE);
    else if(action == ui->actionSearchLogShowInformationAirport)
      emit showInformation(map::MapResult::createFromMapBase(&airport));
    else if(action == ui->actionSearchLogShowOnMapAirport)
    {
      emit showRect(airport.bounding, false);
      NavApp::setStatusMessage(tr("Showing airport on map."));
    }
    // Log airport actions are not connected to any method unlike airport search actions
    else if(action == ui->actionSearchLogRouteAirportStart)
      emit routeSetDeparture(airport);
    else if(action == ui->actionSearchLogRouteAirportDest)
      emit routeSetDestination(airport);
    else if(action == ui->actionSearchLogRouteAirportAlternate)
      emit routeAddAlternate(airport);
    else if(action == ui->actionLogdataRouteOpen)
      emit loadRouteFile(logEntry.routeFile);
    else if(action == ui->actionLogdataPerfLoad)
      emit loadPerfFile(logEntry.perfFile);
    // Logbook actions are not connected to anything - execute here =========
    else if(action == ui->actionSearchLogdataOpenPlan)
      NavApp::getLogdataController()->planOpen(&logRecord, mainWindow);
    else if(action == ui->actionSearchLogdataSavePlanAs)
      NavApp::getLogdataController()->planSaveAs(&logRecord, mainWindow);
    else if(action == ui->actionSearchLogdataOpenPerf)
      NavApp::getLogdataController()->perfOpen(&logRecord, mainWindow);
    else if(action == ui->actionSearchLogdataSavePerfAs)
      NavApp::getLogdataController()->perfSaveAs(&logRecord, mainWindow);
    else if(action == ui->actionSearchLogdataSaveGpxAs)
      NavApp::getLogdataController()->gpxSaveAs(&logRecord, mainWindow);

    // Other actions are connected
  }
}

map::MapAirport SearchBaseTable::currentAirport()
{
  map::MapAirport airport;

  int row = -1;
  QVector<int> rows = getSelectedRows();
  QModelIndex index = view->currentIndex();
  if(!rows.isEmpty())
    // Get topmost airport from selection
    row = rows.constFirst();
  else if(index.isValid())
    // ... otherwise get current at cursor position
    row = index.row();
  else if(getTotalRowCount() > 0)
    // ... or get topmost in result list
    row = 0;
  else
    return airport;

  int id = -1;
  map::MapTypes navType = map::NONE;
  getNavTypeAndId(row, navType, id);

  if(navType == map::AIRPORT && id > 0)
    airportQuery->getAirportById(airport, id);
  return airport;
}

void SearchBaseTable::routeSetDepartureAction()
{
  // Ignore messages from not visible tabs and other tabs
  if(view->isVisible() && tabIndex == si::SEARCH_AIRPORT)
  {
    qDebug() << Q_FUNC_INFO;
    emit routeSetDeparture(currentAirport());
  }
}

void SearchBaseTable::routeSetDestinationAction()
{
  if(view->isVisible() && tabIndex == si::SEARCH_AIRPORT)
  {
    qDebug() << Q_FUNC_INFO;
    emit routeSetDestination(currentAirport());
  }
}

void SearchBaseTable::routeAddAlternateAction()
{
  if(view->isVisible() && tabIndex == si::SEARCH_AIRPORT)
  {
    qDebug() << Q_FUNC_INFO;
    emit routeAddAlternate(currentAirport());
  }
}

/* Triggered by show information action in context menu. Populates map search result and emits show information */
void SearchBaseTable::showInformationTriggered()
{
  if(NavApp::getSearchController()->getCurrentSearchTabId() == tabIndex)
  {
    qDebug() << Q_FUNC_INFO;

    // Index covers a cell
    QModelIndex index = selectedOrFirstIndex();
    if(index.isValid())
    {
      map::MapTypes navType = map::NONE;
      map::MapAirspaceSources airspaceSource = map::AIRSPACE_SRC_NONE;
      int id = -1;
      getNavTypeAndId(index.row(), navType, airspaceSource, id);

      map::MapResult result;
      mapQuery->getMapObjectById(result, navType, airspaceSource, id, false /* airport from nav database */);
      emit showInformation(result);
    }
  }
}

/* Triggered by show approaches action in context menu. Populates map search result and emits show information */
void SearchBaseTable::showApproachesTriggered()
{
  showApproaches(false /* customApproach */, false /* customDeparture */);
}

void SearchBaseTable::showApproachesCustomTriggered()
{
  showApproaches(true /* customApproach */, false /* customDeparture */);
}

void SearchBaseTable::showDeparturesCustomTriggered()
{
  showApproaches(false /* customApproach */, true /* customDeparture */);
}

void SearchBaseTable::showApproaches(bool customApproach, bool customDeparture)
{
  if(NavApp::getSearchController()->getCurrentSearchTabId() == tabIndex)
  {
    qDebug() << Q_FUNC_INFO;

    // Index covers a cell
    QModelIndex index = selectedOrFirstIndex();
    if(index.isValid())
    {
      map::MapTypes navType = map::NONE;
      int id = -1;
      getNavTypeAndId(index.row(), navType, id);

      if(customApproach)
        emit showCustomApproach(airportQuery->getAirportById(id), QString());
      else if(customDeparture)
        emit showCustomDeparture(airportQuery->getAirportById(id), QString());
      else
      {
        map::MapAirport airport = airportQuery->getAirportById(id);
        bool departureFilter, arrivalFilter;
        NavApp::getRouteConst().getAirportProcedureFlags(airport, -1, departureFilter, arrivalFilter);
        emit showProcedures(airport, departureFilter, arrivalFilter);
      }
    }
  }
}

/* Show on map action in context menu */
void SearchBaseTable::showOnMapTriggered()
{
  if(NavApp::getSearchController()->getCurrentSearchTabId() == tabIndex)
  {
    qDebug() << Q_FUNC_INFO;

    QModelIndex index = selectedOrFirstIndex();
    if(index.isValid())
    {
      map::MapTypes navType = map::NONE;
      map::MapAirspaceSources airspaceSource = map::AIRSPACE_SRC_NONE;
      int id = -1;
      getNavTypeAndId(index.row(), navType, airspaceSource, id);

      map::MapResult result;
      mapQuery->getMapObjectById(result, navType, airspaceSource, id, false /* airport from nav database */);

      if(!result.airports.isEmpty())
      {
        emit showRect(result.airports.constFirst().bounding, false);
        NavApp::setStatusMessage(tr("Showing airport on map."));
      }
      else if(!result.airspaces.isEmpty())
      {
        emit showRect(result.airspaces.constFirst().bounding, false);
        NavApp::setStatusMessage(tr("Showing airspace on map."));
      }
      else if(!result.vors.isEmpty())
      {
        emit showPos(result.vors.constFirst().getPosition(), 0.f, false);
        NavApp::setStatusMessage(tr("Showing VOR on map."));
      }
      else if(!result.ndbs.isEmpty())
      {
        emit showPos(result.ndbs.constFirst().getPosition(), 0.f, false);
        NavApp::setStatusMessage(tr("Showing NDB on map."));
      }
      else if(!result.waypoints.isEmpty())
      {
        emit showPos(result.waypoints.constFirst().getPosition(), 0.f, false);
        NavApp::setStatusMessage(tr("Showing waypoint on map."));
      }
      else if(!result.userpoints.isEmpty())
      {
        emit showPos(result.userpoints.constFirst().getPosition(), 0.f, false);
        NavApp::setStatusMessage(tr("Showing userpoint on map."));
      }
      else if(!result.logbookEntries.isEmpty())
      {
        emit showRect(result.logbookEntries.constFirst().bounding(), false);
        NavApp::setStatusMessage(tr("Showing logbook entry on map."));
      }
      else if(!result.onlineAircraft.isEmpty())
      {
        emit showPos(result.onlineAircraft.constFirst().getPosition(), 0.f, false);
        NavApp::setStatusMessage(tr("Showing online client/aircraft on map."));
      }
    }
  }
}

QModelIndex SearchBaseTable::selectedOrFirstIndex()
{
  QModelIndex idx = view->currentIndex();
  if(!idx.isValid())
    idx = view->model()->index(0, 0);
  return idx;
}

void SearchBaseTable::getNavTypeAndId(int row, map::MapTypes& navType, int& id)
{
  map::MapAirspaceSources airspaceSource;
  getNavTypeAndId(row, navType, airspaceSource, id);
}

/* Fetch nav type and database id from a model row */
void SearchBaseTable::getNavTypeAndId(int row, map::MapTypes& navType, map::MapAirspaceSources& airspaceSource,
                                      int& id)
{
  navType = map::NONE;
  id = -1;
  airspaceSource = map::AIRSPACE_SRC_NONE;

  if(tabIndex == si::SEARCH_AIRPORT)
  {
    // Airport table
    navType = map::AIRPORT;
    id = controller->getRawData(row, columns->getIdColumn()->getIndex()).toInt();
  }
  else if(tabIndex == si::SEARCH_NAV)
  {
    // Otherwise nav_search table
    navType = map::navTypeToMapObjectType(controller->getRawData(row, "nav_type").toString());

    if(navType == map::VOR)
      id = controller->getRawData(row, "vor_id").toInt();
    else if(navType == map::NDB)
      id = controller->getRawData(row, "ndb_id").toInt();
    else if(navType == map::WAYPOINT)
      id = controller->getRawData(row, "waypoint_id").toInt();
  }
  else if(tabIndex == si::SEARCH_USER)
  {
    // User data
    navType = map::USERPOINT;
    id = controller->getRawData(row, columns->getIdColumn()->getIndex()).toInt();
  }
  else if(tabIndex == si::SEARCH_LOG)
  {
    // Logbook
    navType = map::LOGBOOK;
    id = controller->getRawData(row, columns->getIdColumn()->getIndex()).toInt();
  }
  else if(tabIndex == si::SEARCH_ONLINE_CLIENT)
  {
    navType = map::AIRCRAFT_ONLINE;
    id = controller->getRawData(row, columns->getIdColumn()->getIndex()).toInt();
  }
  else if(tabIndex == si::SEARCH_ONLINE_CENTER)
  {
    navType = map::AIRSPACE;
    airspaceSource = map::AIRSPACE_SRC_ONLINE;
    id = controller->getRawData(row, columns->getIdColumn()->getIndex()).toInt();
  }
  else if(tabIndex == si::SEARCH_ONLINE_SERVER)
  {
    navType = map::NONE;
  }
}

void SearchBaseTable::tabDeactivated()
{
  emit selectionChanged(this, 0, controller->getVisibleRowCount(), controller->getTotalRowCount());
}

/* Callback for the controller. Will be called for each table cell and should return a formatted value */
QVariant SearchBaseTable::modelDataHandler(int colIndex, int rowIndex, const Column *col, const QVariant&,
                                           const QVariant& displayRoleValue, Qt::ItemDataRole role) const
{
  switch(role)
  {
    case Qt::DisplayRole:
      return formatModelData(col, displayRoleValue);

    case Qt::TextAlignmentRole:
      if(displayRoleValue.type() == QVariant::Int || displayRoleValue.type() == QVariant::UInt ||
         displayRoleValue.type() == QVariant::LongLong || displayRoleValue.type() == QVariant::ULongLong ||
         displayRoleValue.type() == QVariant::Double)
        // Align all numeric columns right
        return Qt::AlignRight;

      break;
    case Qt::BackgroundRole:
      if(colIndex == controller->getSortColumnIndex())
        // Use another alternating color if this is a field in the sort column
        return mapcolors::alternatingRowColor(rowIndex, true);

      break;
    default:
      break;
  }

  return QVariant();
}

/* Formats the QVariant to a QString depending on column name */
QString SearchBaseTable::formatModelData(const Column *, const QVariant& displayRoleValue) const
{
  // Called directly by the model for export functions
  if(displayRoleValue.type() == QVariant::Int || displayRoleValue.type() == QVariant::UInt)
    return QLocale().toString(displayRoleValue.toInt());
  else if(displayRoleValue.type() == QVariant::LongLong || displayRoleValue.type() == QVariant::ULongLong)
    return QLocale().toString(displayRoleValue.toLongLong());
  else if(displayRoleValue.type() == QVariant::Double)
    return QLocale().toString(displayRoleValue.toDouble());

  return displayRoleValue.toString();
}

void SearchBaseTable::selectAll()
{
  view->selectAll();
}
