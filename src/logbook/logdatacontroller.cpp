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

#include "logbook/logdatacontroller.h"

#include "atools.h"
#include "common/constants.h"
#include "common/maptypes.h"
#include "common/maptypesfactory.h"
#include "exception.h"
#include "fs/userdata/logdatamanager.h"
#include "gui/dialog.h"
#include "gui/errorhandler.h"
#include "gui/helphandler.h"
#include "gui/mainwindow.h"
#include "gui/textdialog.h"
#include "perf/aircraftperfcontroller.h"
#include "search/searchcontroller.h"
#include "logbook/logdataconverter.h"
#include "common/aircrafttrack.h"
#include "logbook/logdatadialog.h"
#include "logbook/logstatisticsdialog.h"
#include "zip/gzip.h"
#include "navapp.h"
#include "query/airportquery.h"
#include "route/route.h"
#include "route/routealtitude.h"
#include "search/logdatasearch.h"
#include "settings/settings.h"
#include "sql/sqlrecord.h"
#include "sql/sqltransaction.h"
#include "ui_mainwindow.h"
#include "util/htmlbuilder.h"
#include "options/optiondata.h"
#include "fs/pln/flightplanio.h"
#include "fs/perf/aircraftperf.h"
#include "gui/choicedialog.h"
#include "gui/errorhandler.h"

#include <QDebug>
#include <QStandardPaths>

using atools::sql::SqlTransaction;
using atools::sql::SqlRecord;
using atools::geo::Pos;
using atools::fs::pln::FlightplanIO;

LogdataController::LogdataController(atools::fs::userdata::LogdataManager *logdataManager, MainWindow *parent)
  : manager(logdataManager), mainWindow(parent)
{
  dialog = new atools::gui::Dialog(mainWindow);

  // Do not use a parent to allow the window moving to back
  statsDialog = new LogStatisticsDialog(nullptr, this);

  // Add to dock handler to enable auto raise and closing on exit
  NavApp::addDialogToDockHandler(statsDialog);

  connect(this, &LogdataController::logDataChanged, statsDialog, &LogStatisticsDialog::logDataChanged);
  connect(this, &LogdataController::logDataChanged, manager, &atools::sql::DataManagerBase::updateUndoRedoActions);

  Ui::MainWindow *ui = NavApp::getMainUi();
  connect(ui->actionSearchLogdataUndo, &QAction::triggered, this, &LogdataController::undoTriggered);
  connect(ui->actionSearchLogdataRedo, &QAction::triggered, this, &LogdataController::redoTriggered);

  manager->setMaximumUndoSteps(50);
  manager->setTextSuffix(tr(" Logbook Entry"), tr(" Logbook Entries"));
  manager->setActions(ui->actionSearchLogdataUndo, ui->actionSearchLogdataRedo);
}

LogdataController::~LogdataController()
{
  NavApp::removeDialogFromDockHandler(statsDialog);
  delete statsDialog;
  delete aircraftAtTakeoff;
  delete dialog;
}

void LogdataController::undoTriggered()
{
  qDebug() << Q_FUNC_INFO;
  if(manager->canUndo())
  {
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    manager->undo();
    QGuiApplication::restoreOverrideCursor();

    manager->clearGeometryCache();

    emit refreshLogSearch(false, false);
    emit logDataChanged();
  }
}

void LogdataController::redoTriggered()
{
  qDebug() << Q_FUNC_INFO;
  if(manager->canRedo())
  {
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    manager->redo();
    QGuiApplication::restoreOverrideCursor();

    manager->clearGeometryCache();

    emit refreshLogSearch(false, false);
    emit logDataChanged();
  }
}

void LogdataController::showSearch()
{
  Ui::MainWindow *ui = NavApp::getMainUi();
  ui->dockWidgetSearch->show();
  ui->dockWidgetSearch->raise();
  NavApp::getSearchController()->setCurrentSearchTabId(si::SEARCH_LOG);
}

void LogdataController::saveState()
{
  // Not used yet
  atools::settings::Settings::instance().setValue(lnm::LOGDATA_ENTRY_ID, logEntryId);
}

void LogdataController::restoreState()
{
  // logEntryId = atools::settings::Settings::instance().valueInt(lnm::LOGDATA_ENTRY_ID);
  manager->updateUndoRedoActions();
}

void LogdataController::optionsChanged()
{
  statsDialog->optionsChanged();
}

void LogdataController::deleteLogEntryFromMap(int id)
{
  deleteLogEntries({id});
}

map::MapLogbookEntry LogdataController::getLogEntryById(int id)
{
  map::MapLogbookEntry obj;
  MapTypesFactory().fillLogbookEntry(manager->getRecord(id), obj);
  return obj;
}

atools::sql::SqlRecord LogdataController::getLogEntryRecordById(int id)
{
  return manager->getRecord(id);
}

void LogdataController::getFlightStatsTime(QDateTime& earliest, QDateTime& latest, QDateTime& earliestSim,
                                           QDateTime& latestSim)
{
  manager->getFlightStatsTime(earliest, latest, earliestSim, latestSim);
}

void LogdataController::getFlightStatsDistance(float& distTotal, float& distMax, float& distAverage)
{
  manager->getFlightStatsDistance(distTotal, distMax, distAverage);
}

void LogdataController::getFlightStatsAirports(int& numDepartAirports, int& numDestAirports)
{
  manager->getFlightStatsAirports(numDepartAirports, numDestAirports);
}

void LogdataController::getFlightStatsTripTime(float& timeMaximum, float& timeAverage, float& timeTotal,
                                               float& timeMaximumSim, float& timeAverageSim, float& timeTotalSim)
{
  manager->getFlightStatsTripTime(timeMaximum, timeAverage, timeTotal, timeMaximumSim, timeAverageSim, timeTotalSim);
}

void LogdataController::getFlightStatsAircraft(int& numTypes, int& numRegistrations, int& numNames, int& numSimulators)
{
  manager->getFlightStatsAircraft(numTypes, numRegistrations, numNames, numSimulators);
}

void LogdataController::getFlightStatsSimulator(QVector<std::pair<int, QString> >& numSimulators)
{
  manager->getFlightStatsSimulator(numSimulators);
}

void LogdataController::statisticsLogbookShow()
{
  statsDialog->show();
  statsDialog->raise();
  statsDialog->activateWindow();
}

atools::sql::SqlDatabase *LogdataController::getDatabase() const
{
  return manager->getDatabase();
}

void LogdataController::aircraftTakeoff(const atools::fs::sc::SimConnectUserAircraft& aircraft)
{
  createTakeoffLanding(aircraft, true /*takeoff*/, 0.f);
}

void LogdataController::aircraftLanding(const atools::fs::sc::SimConnectUserAircraft& aircraft, float flownDistanceNm)
{
  createTakeoffLanding(aircraft, false /*takeoff*/, flownDistanceNm);
}

void LogdataController::createTakeoffLanding(const atools::fs::sc::SimConnectUserAircraft& aircraft, bool takeoff, float flownDistanceNm)
{
  if(NavApp::getMainUi()->actionLogdataCreateLogbook->isChecked())
  {
    // Get nearest airport on takeoff/landing and runway
    map::MapRunwayEnd runwayEnd;
    map::MapAirport airport;
    if(!NavApp::getAirportQuerySim()->getBestRunwayEndForPosAndCourse(runwayEnd, airport,
                                                                      aircraft.getPosition(),
                                                                      aircraft.getTrackDegTrue()))
    {
      // Not even an airport was found
      qWarning() << Q_FUNC_INFO << "No runway found for aircraft"
                 << aircraft.getPosition() << aircraft.getTrackDegTrue();
      return;
    }

    QString departureArrivalText = takeoff ? tr("Departure") : tr("Arrival");
    QString runwayText = runwayEnd.isValid() ? tr(" runway %1").arg(runwayEnd.name) : QString();

    if(takeoff)
    {
      // Build record for new logbook entry =======================================
      SqlRecord record = manager->getEmptyRecord();
      record.setValue("aircraft_name", aircraft.getAirplaneType()); // varchar(250),
      record.setValue("aircraft_type", aircraft.getAirplaneModel()); // varchar(250),
      record.setValue("aircraft_registration", aircraft.getAirplaneRegistration()); // varchar(50),
      record.setValue("flightplan_number", aircraft.getAirplaneFlightnumber()); // varchar(100),
      record.setValue("flightplan_cruise_altitude", NavApp::getRouteCruiseAltFt()); // integer,
      record.setValue("flightplan_file", NavApp::getCurrentRouteFilepath()); // varchar(1024),
      record.setValue("performance_file", NavApp::getCurrentAircraftPerfFilepath()); // varchar(1024),
      record.setValue("block_fuel", NavApp::getAltitudeLegs().getBlockFuel(NavApp::getAircraftPerformance())); // integer,
      record.setValue("trip_fuel", NavApp::getAltitudeLegs().getTripFuel()); // integer,
      record.setValue("grossweight", aircraft.getAirplaneTotalWeightLbs()); // integer,
      record.setValue("departure_ident", airport.displayIdent()); // varchar(10),
      record.setValue("departure_name", airport.name); // varchar(200),
      record.setValue("departure_runway", runwayEnd.name); // varchar(200),
      record.setValue("departure_lonx", airport.position.getLonX()); // integer,
      record.setValue("departure_laty", airport.position.getLatY()); // integer,
      record.setValue("departure_alt", airport.position.getAltitude()); // integer,

      // Sqlite TEXT as ISO8601 strings (2021-05-16T23:55:00.259+02:00).
      // TEXT as ISO8601 strings ("YYYY-MM-DD HH:MM:SS.SSS")
      record.setValue("departure_time", atools::currentIsoWithOffset()); // varchar(100),

      // 2021-05-16T21:50:24.973Z
      record.setValue("departure_time_sim", aircraft.getZuluTime()); // varchar(100),

      record.setValue("simulator", NavApp::getCurrentSimulatorShortName()); // varchar(50),
      record.setValue("route_string", NavApp::getRouteString()); // varchar(1024),

      // Clear separate logbook track =========================
      NavApp::deleteAircraftTrackLogbook();

      // Record flight plan and aircraft performance =========================
      recordFlightplanAndPerf(record);

      // Determine fuel type =========================
      float weightVolRatio = 0.f;
      bool jetfuel = aircraft.isJetfuel(weightVolRatio);
      if(weightVolRatio > 0.f)
        record.setValue("is_jetfuel", jetfuel); // integer,

      // Add to database and remember created id
      SqlTransaction transaction(manager->getDatabase());
      manager->insertOneRecord(record);
      logEntryId = manager->getCurrentId();
      transaction.commit();

      logChanged(false /* load all */, true /* keep selection */);

      mainWindow->setStatusMessage(tr("Logbook Entry for %1 at %2%3 added.").
                                   arg(departureArrivalText).
                                   arg(airport.displayIdent()).
                                   arg(runwayText), true /* addToLog */);
    }
    else if(logEntryId >= 0)
    {
      if(manager->hasRecord(logEntryId))
      {
        // Update takeoff record with landing data ===========================================
        atools::sql::SqlRecord record = manager->getRecord(logEntryId);
        record.setValue("distance", NavApp::getRouteConst().getTotalDistance()); // integer,
        record.setValue("distance_flown", flownDistanceNm); // integer,
        if(aircraftAtTakeoff != nullptr)
          record.setValue("used_fuel", aircraftAtTakeoff->getFuelTotalWeightLbs() - aircraft.getFuelTotalWeightLbs()); // integer,
        record.setValue("destination_ident", airport.displayIdent()); // varchar(10),
        record.setValue("destination_name", airport.name); // varchar(200),
        record.setValue("destination_runway", runwayEnd.name); // varchar(200),
        record.setValue("destination_lonx", airport.position.getLonX()); // integer,
        record.setValue("destination_laty", airport.position.getLatY()); // integer,
        record.setValue("destination_alt", airport.position.getAltitude()); // integer,

        // Sqlite TEXT as ISO8601 strings (2021-05-16T23:55:00.259+02:00).
        // TEXT as ISO8601 strings ("YYYY-MM-DD HH:MM:SS.SSS")
        record.setValue("destination_time", atools::currentIsoWithOffset()); // varchar(100),

        // 2021-05-16T21:50:24.973Z
        record.setValue("destination_time_sim", aircraft.getZuluTime()); // varchar(100),

        // Update flight plan and aircraft performance =========================
        recordFlightplanAndPerf(record);

        // Save GPX with simplified flight plan and trail =========================
        record.setValue("aircraft_trail",
                        FlightplanIO().saveGpxGz(NavApp::getRouteConst().
                                                 updatedAltitudes().adjustedToOptions(rf::DEFAULT_OPTS_GPX).
                                                 getFlightplan(),
                                                 NavApp::getAircraftTrackLogbook().getLineStrings(),
                                                 NavApp::getAircraftTrackLogbook().getTimestampsMs(),
                                                 static_cast<int>(NavApp::getRouteConst().getCruisingAltitudeFeet()))); // blob

        // Clear separate logbook track =========================
        NavApp::deleteAircraftTrackLogbook();

        // Determine fuel type again =========================
        float weightVolRatio = 0.f;
        bool jetfuel = aircraft.isJetfuel(weightVolRatio);
        if(weightVolRatio > 0.f)
          record.setValue("is_jetfuel", jetfuel); // integer,

        SqlTransaction transaction(manager->getDatabase());
        manager->updateRecords(record, {logEntryId});
        transaction.commit();

        logChanged(false /* load all */, false /* keep selection */);

        mainWindow->setStatusMessage(tr("Logbook Entry for %1 at %2%3 updated.").
                                     arg(departureArrivalText).
                                     arg(airport.displayIdent()).
                                     arg(runwayText), true /* addToLog */);

        logEntryId = -1;
      }
      else
        qWarning() << Q_FUNC_INFO << "Log entry ID not found" << logEntryId;
    }
    else
      qWarning() << Q_FUNC_INFO << "no previous takeoff for ID" << logEntryId;
  }

  resetTakeoffLandingDetection();

  if(takeoff)
    aircraftAtTakeoff = new atools::fs::sc::SimConnectUserAircraft(aircraft);
}

void LogdataController::logChanged(bool loadAll, bool keepSelection)
{
  // Clear cache and update map screen index
  manager->clearGeometryCache();
  manager->updateUndoRedoActions();

  emit logDataChanged();

  // Reload search
  emit refreshLogSearch(loadAll, keepSelection);
}

void LogdataController::recordFlightplanAndPerf(atools::sql::SqlRecord& record)
{
  atools::fs::pln::Flightplan fp = NavApp::getRouteConst().
                                   updatedAltitudes().adjustedToOptions(rf::DEFAULT_OPTS_LNMPLN).getFlightplan();

  if(fp.isEmpty())
    record.setNull("flightplan"); // no plan
  else
    record.setValue("flightplan", FlightplanIO().saveLnmGz(fp)); // blob

  record.setValue("aircraft_perf", NavApp::getAircraftPerformance().saveXmlGz()); // blob
}

void LogdataController::resetTakeoffLandingDetection()
{
  delete aircraftAtTakeoff;
  aircraftAtTakeoff = nullptr;
}

bool LogdataController::isDirectPreviewShown()
{
  return NavApp::getMainUi()->actionSearchLogdataShowDirect->isChecked();
}

bool LogdataController::isRoutePreviewShown()
{
  return NavApp::getMainUi()->actionSearchLogdataShowRoute->isChecked();
}

bool LogdataController::isTrackPreviewShown()
{
  return NavApp::getMainUi()->actionSearchLogdataShowTrack->isChecked();
}

bool LogdataController::hasRouteAttached(int id)
{
  return manager->hasRouteAttached(id);
}

bool LogdataController::hasPerfAttached(int id)
{
  return manager->hasPerfAttached(id);
}

bool LogdataController::hasTrackAttached(int id)
{
  return manager->hasTrackAttached(id);
}

void LogdataController::preDatabaseLoad()
{
  // no-op
}

void LogdataController::postDatabaseLoad()
{
  manager->clearGeometryCache();
}

void LogdataController::displayOptionsChanged()
{
  manager->clearGeometryCache();
}

const atools::geo::LineString *LogdataController::getRouteGeometry(int id)
{
  const atools::fs::userdata::LogEntryGeometry *entry = manager->getGeometry(id);
  return entry != nullptr ? &entry->route : nullptr;
}

const QVector<atools::geo::LineString> *LogdataController::getTrackGeometry(int id)
{
  const atools::fs::userdata::LogEntryGeometry *entry = manager->getGeometry(id);
  return entry != nullptr ? &entry->tracks : nullptr;
}

void LogdataController::editLogEntryFromMap(int id)
{
  qDebug() << Q_FUNC_INFO;
  editLogEntries({id});
}

void LogdataController::connectDialogSignals(LogdataDialog *dialogParam)
{
  connect(dialogParam, &LogdataDialog::planOpen, this, &LogdataController::planOpen);
  connect(dialogParam, &LogdataDialog::planAdd, this, &LogdataController::planAdd);
  connect(dialogParam, &LogdataDialog::planSaveAs, this, &LogdataController::planSaveAs);
  connect(dialogParam, &LogdataDialog::gpxAdd, this, &LogdataController::gpxAdd);
  connect(dialogParam, &LogdataDialog::gpxSaveAs, this, &LogdataController::gpxSaveAs);
  connect(dialogParam, &LogdataDialog::perfOpen, this, &LogdataController::perfOpen);
  connect(dialogParam, &LogdataDialog::perfAdd, this, &LogdataController::perfAdd);
  connect(dialogParam, &LogdataDialog::perfSaveAs, this, &LogdataController::perfSaveAs);
}

void LogdataController::editLogEntries(const QVector<int>& ids)
{
  qDebug() << Q_FUNC_INFO << ids;

  SqlRecord rec = manager->getRecord(ids.constFirst());
  if(!rec.isEmpty())
  {
    LogdataDialog dlg(mainWindow, ids.size() == 1 ? ld::EDIT_ONE : ld::EDIT_MULTIPLE);
    connectDialogSignals(&dlg);
    dlg.restoreState();

#ifdef DEBUG_INFORMATION
    qDebug() << Q_FUNC_INFO << rec;
    qDebug() << Q_FUNC_INFO << ids;
#endif

    dlg.setRecord(rec);
    int retval = dlg.exec();
    if(retval == QDialog::Accepted)
    {
      // Change modified columns for all given ids
      SqlTransaction transaction(manager->getDatabase());
      manager->updateRecords(dlg.getRecord(), ids);
      transaction.commit();

      logChanged(false /* load all */, true /* keep selection */);

      mainWindow->setStatusMessage(tr("%1 logbook %2 updated.").arg(ids.size()).arg(ids.size() == 1 ? tr("entry") : tr("entries")));
    }
    dlg.saveState();
  }
  else
    qWarning() << Q_FUNC_INFO << "Empty record" << rec;
}

void LogdataController::prefillLogEntry(atools::sql::SqlRecord& rec)
{
  const Route& route = NavApp::getRouteConst();

  const atools::fs::sc::SimConnectUserAircraft& aircraft = NavApp::getUserAircraft();
  if(aircraft.isValid())
  {
    rec.setValue("aircraft_name", aircraft.getAirplaneType()); // varchar(250),
    rec.setValue("aircraft_type", aircraft.getAirplaneModel()); // varchar(250),
    rec.setValue("aircraft_registration", aircraft.getAirplaneRegistration()); // varchar(50),
    rec.setValue("flightplan_number", aircraft.getAirplaneFlightnumber()); // varchar(100),
  }

  rec.setValue("aircraft_type", NavApp::getAircraftPerformance().getAircraftType());
  rec.setValue("simulator", NavApp::getCurrentSimulatorName());
  rec.setValue("route_string", NavApp::getRouteString());
  rec.setValue("flightplan_cruise_altitude", NavApp::getRouteCruiseAltFt());
  rec.setValue("block_fuel", NavApp::getAircraftPerfController()->getBlockFuel());
  rec.setValue("trip_fuel", NavApp::getAircraftPerfController()->getTripFuel());
  rec.setValue("is_jetfuel", NavApp::getAircraftPerfController()->getAircraftPerformance().isJetFuel());
  rec.setValue("distance", route.getTotalDistance());

  // Departure information =============================================
  QString departRw, destRw;
  route.getRunwayNames(departRw, destRw);
  if(route.hasValidDeparture())
  {
    const RouteLeg& departureAirportLeg = route.getDepartureAirportLeg();
    rec.setValue("departure_ident", departureAirportLeg.getIdent());
    rec.setValue("departure_name", departureAirportLeg.getName());
    rec.setValue("departure_runway", departRw);
    rec.setValue("departure_lonx", departureAirportLeg.getPosition().getLonX());
    rec.setValue("departure_laty", departureAirportLeg.getPosition().getLatY());
    rec.setValue("departure_alt", departureAirportLeg.getAltitude()); // integer,
  }

  // Destination information =============================================
  if(route.hasValidDestination())
  {
    const RouteLeg& destinationAirportLeg = route.getDestinationAirportLeg();
    rec.setValue("destination_ident", destinationAirportLeg.getIdent());
    rec.setValue("destination_name", destinationAirportLeg.getName());
    rec.setValue("destination_runway", destRw);
    rec.setValue("destination_lonx", destinationAirportLeg.getPosition().getLonX());
    rec.setValue("destination_laty", destinationAirportLeg.getPosition().getLatY());
    rec.setValue("destination_alt", destinationAirportLeg.getAltitude()); // integer,
  }

  // File attachements =============================================

  planAttachLnmpln(&rec, NavApp::getCurrentRouteFilepath(), mainWindow);
  perfAttachLnmperf(&rec, NavApp::getCurrentAircraftPerfFilepath(), mainWindow);
  gpxAttach(&rec, mainWindow, true /* currentTrack */);

  // Filenames ======================================================
  rec.setValue("flightplan_file", NavApp::getCurrentRouteFilepath());
  rec.setValue("performance_file", NavApp::getCurrentAircraftPerfFilepath());
}

void LogdataController::addLogEntry()
{
  qDebug() << Q_FUNC_INFO;

  SqlRecord rec = manager->getEmptyRecord();

  // Prefill record for add dialog with values from current program state
  prefillLogEntry(rec);

#ifdef DEBUG_INFORMATION
  qDebug() << Q_FUNC_INFO << rec;
#endif

  LogdataDialog dlg(mainWindow, ld::ADD);
  connectDialogSignals(&dlg);
  dlg.restoreState();

  dlg.setRecord(rec);
  int retval = dlg.exec();
  if(retval == QDialog::Accepted)
  {
#ifdef DEBUG_INFORMATION
    qDebug() << Q_FUNC_INFO << rec;
#endif

    // Add to database
    SqlTransaction transaction(manager->getDatabase());
    manager->insertOneRecord(dlg.getRecord());
    transaction.commit();

    logChanged(false /* load all */, false /* keep selection */);

    mainWindow->setStatusMessage(tr("Logbook entry added."));
  }
  dlg.saveState();
}

void LogdataController::deleteLogEntries(const QVector<int>& ids)
{
  qDebug() << Q_FUNC_INFO;

  QString txt = ids.size() == 1 ? tr("entry") : tr("entries");
  SqlTransaction transaction(manager->getDatabase());
  manager->deleteRows(ids);
  transaction.commit();

  logChanged(false /* load all */, false /* keep selection */);

  mainWindow->setStatusMessage(tr("%1 logbook %2 deleted.").arg(ids.size()).arg(txt));
}

void LogdataController::importXplane()
{
  qDebug() << Q_FUNC_INFO;
  try
  {
    QString xpBasePath = NavApp::getSimulatorBasePath(atools::fs::FsPaths::XPLANE_12);
    if(xpBasePath.isEmpty())
      xpBasePath = NavApp::getSimulatorBasePath(atools::fs::FsPaths::XPLANE_11);

    if(xpBasePath.isEmpty())
      xpBasePath = atools::documentsDir();
    else
      xpBasePath = atools::buildPathNoCase({xpBasePath, "Output", "logbooks"});

    QString file = dialog->openFileDialog(
      tr("Open X-Plane Logbook File"),
      tr("X-Plane Logbook Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_XPLANE_LOGBOOK), "Logdata/XPlane",
      xpBasePath);

    int numImported = 0;
    if(!file.isEmpty())
    {
      numImported += manager->importXplane(file, fetchAirportCoordinates);
      mainWindow->setStatusMessage(tr("Imported %1 %2 X-Plane logbook.").arg(numImported).
                                   arg(numImported == 1 ? tr("entry") : tr("entries")));

      logChanged(false /* load all */, false /* keep selection */);

      // Enable more search options to show user the search criteria
      NavApp::getMainUi()->actionLogdataSearchShowMoreOptions->setChecked(true);

      /*: The text "Imported from X-Plane logbook" has to match the one in atools::fs::userdata::LogdataManager::importXplane */
      emit showInSearch(map::LOGBOOK,
                        atools::sql::SqlRecord().appendFieldAndValue("description",
                                                                     tr("*Imported from X-Plane logbook*")),
                        false /* select */);
    }
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
  }
}

void LogdataController::importCsv()
{
  qDebug() << Q_FUNC_INFO;
  try
  {
    QString file = dialog->openFileDialog(
      tr("Open Logbook CSV File"),
      tr("CSV Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_USERDATA_CSV), "Logdata/Csv");

    int numImported = 0;
    if(!file.isEmpty())
    {
      numImported += manager->importCsv(file);
      mainWindow->setStatusMessage(tr("Imported %1 %2 from CSV file.").arg(numImported).
                                   arg(numImported == 1 ? tr("entry") : tr("entries")));
      mainWindow->showLogbookSearch();
      logChanged(false /* load all */, false /* keep selection */);
    }
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
  }
}

void LogdataController::exportCsv()
{

  qDebug() << Q_FUNC_INFO;
  try
  {
    // Checkbox ids
    enum
    {
      SELECTED, APPEND, HEADER, EXPORTPLAN, EXPORTPERF, EXPORTGPX
    };

    // Build a choice dialog with several checkboxes =========================
    atools::gui::ChoiceDialog choiceDialog(mainWindow, QApplication::applicationName() + tr(" - Logbook Export"),
                                           tr("Select export options for logbook"), lnm::LOGDATA_EXPORT_CSV,
                                           "LOGBOOK.html#import-and-export");
    choiceDialog.setHelpOnlineUrl(lnm::helpOnlineUrl);
    choiceDialog.setHelpLanguageOnline(lnm::helpLanguageOnline());

    QString attachmentToolTip = tr("Content of attached file will be added to the exported CSV if selected.\n"
                                   "Note that not all programs will be able to read this.\n"
                                   "Columns will be empty on export if disabled.");

    int numSelected = NavApp::getLogdataSearch()->getSelectedRowCount();
    choiceDialog.addCheckBox(APPEND, tr("&Append to an already present file"),
                             tr("File header will be ignored if this is enabled."), false);
    choiceDialog.addCheckBox(SELECTED, tr("Export &selected entries only"), QString(), true,
                             numSelected == 0 /* disabled */);
    choiceDialog.addCheckBox(HEADER, tr("Add a &header to the first line"), QString(), false);
    choiceDialog.addLine();
    choiceDialog.addCheckBox(EXPORTPLAN, tr("&Flight plan in LNMPLN format"), attachmentToolTip, false);
    choiceDialog.addCheckBox(EXPORTPERF, tr("&Aircraft performance"), attachmentToolTip, false);
    choiceDialog.addCheckBox(EXPORTGPX, tr(
                               "&GPX file containing flight plan points and trail"), attachmentToolTip, false);
    choiceDialog.addSpacer();
    choiceDialog.restoreState();

    // Disable/enable header depending on append option
    choiceDialog.getCheckBox(HEADER)->setDisabled(choiceDialog.isChecked(APPEND));
    atools::gui::ChoiceDialog *dlgPtr = &choiceDialog;
    connect(&choiceDialog, &atools::gui::ChoiceDialog::checkBoxToggled, this, [dlgPtr](int id, bool checked) {
      if(id == APPEND)
        dlgPtr->getCheckBox(HEADER)->setDisabled(checked);
    });

    if(choiceDialog.exec() == QDialog::Accepted)
    {
      QString file = dialog->saveFileDialog(
        tr("Export Logbook Entry CSV File"),
        tr("CSV Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_USERDATA_CSV), ".csv", "Logdata/Csv",
        QString(), QString(), choiceDialog.isChecked(APPEND));

      if(!file.isEmpty())
      {
        QVector<int> ids;
        if(choiceDialog.isChecked(SELECTED))
          ids = NavApp::getLogdataSearch()->getSelectedIds();

        int numExported = manager->exportCsv(file, ids,
                                             choiceDialog.isChecked(EXPORTPLAN),
                                             choiceDialog.isChecked(EXPORTPERF),
                                             choiceDialog.isChecked(EXPORTGPX),
                                             choiceDialog.isChecked(HEADER) && !choiceDialog.isChecked(APPEND),
                                             choiceDialog.isChecked(APPEND));
        mainWindow->setStatusMessage(tr("%1 logbook %2 exported.").
                                     arg(numExported).arg(numExported == 1 ? tr("entry") : tr("entries")));
      }
    }
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
  }
}

void LogdataController::convertUserdata()
{
  qDebug() << Q_FUNC_INFO;

  int result = dialog->showQuestionMsgBox(lnm::ACTIONS_SHOW_LOGBOOK_CONVERSION,
                                          tr("This will convert all userpoints of type "
                                             "\"Logbook\" to logbook entries.<br/><br/>"
                                             "This works best if you did not modify the field "
                                             "\"Description\" in the userpoints and if "
                                             "you did not insert entries manually.<br/><br/>"
                                             "Note that not all fields can be converted automatically.<br/><br/>"
                                             "The created log entries can be found by searching"
                                             "for<br/>\"*Converted from userdata*\"<br/>"
                                             "in the field &quot;Remarks&quot;.<br/><br/>"
                                             "Continue?"),
                                          tr("Do not &show this dialog again and run the conversion in the future."),
                                          QMessageBox::Yes | QMessageBox::No | QMessageBox::Help,
                                          QMessageBox::No, QMessageBox::Yes);

  if(result == QMessageBox::Yes)
  {
    LogdataConverter converter(NavApp::getDatabaseUser(), manager, NavApp::getAirportQuerySim());

    QGuiApplication::setOverrideCursor(Qt::WaitCursor);

    // Do the conversion ===================================
    int numCreated = converter.convertFromUserdata();

    QString resultText = tr("Created %1 log entries.").arg(numCreated);

    if(!converter.getErrors().isEmpty())
    {
      // Show errors and warnings ======================
      atools::util::HtmlBuilder html(true);

      html.p(tr("Logbook Conversion"), atools::util::html::BOLD | atools::util::html::BIG);

      html.p(resultText);

      html.p(tr("Conversion Errors/Warnings"), atools::util::html::BOLD | atools::util::html::BIG);
      html.p(tr("Some warnings might appear because of terminated flights, "
                "repeated langings and/or takeoffs. These can be ignored."));
      html.ol();
      for(const QString& err : converter.getErrors())
        html.li(err, atools::util::html::NO_ENTITIES);
      html.olEnd();

      TextDialog error(mainWindow, QApplication::applicationName() + tr(" - Conversion Errors"),
                       "LOGBOOK.html#convert-errors"); // anchor for future use
      error.setHtmlMessage(html.getHtml(), true /* print to log */);
      QGuiApplication::restoreOverrideCursor();
      error.exec();
    }
    else
    {
      // No errors ======================
      QGuiApplication::restoreOverrideCursor();
      QMessageBox::information(mainWindow, QApplication::applicationName(), resultText);
    }

    mainWindow->showLogbookSearch();

    logChanged(false /* load all */, false /* keep selection */);

    // Enable more search options to show user the search criteria
    NavApp::getMainUi()->actionLogdataSearchShowMoreOptions->setChecked(true);

    /*: The text "Converted from userdata" has to match the one in LogdataConverter::convertFromUserdata */
    emit showInSearch(map::LOGBOOK,
                      atools::sql::SqlRecord().appendFieldAndValue("description", tr("*Converted from userdata*")),
                      false /* select */);
  }
  else if(result == QMessageBox::Help)
    atools::gui::HelpHandler::openHelpUrlWeb(mainWindow, lnm::helpOnlineUrl + "LOGBOOK.html#convert",
                                             lnm::helpLanguageOnline());
}

void LogdataController::fetchAirportCoordinates(atools::geo::Pos& pos, QString& name, const QString& airportIdent)
{
  map::MapAirport airport = NavApp::getAirportQuerySim()->getAirportByIdent(airportIdent);

  if(airport.isValid())
  {
    pos = airport.position;
    name = airport.name;
  }
}

void LogdataController::planOpen(atools::sql::SqlRecord *record, QWidget *parent)
{
  try
  {
    qDebug() << Q_FUNC_INFO;
    // Open flight plan in table replacing the current plan - attachement is always LNMPLN
    mainWindow->routeOpenFileLnmStr(QString(atools::zip::gzipDecompress(record->value("flightplan").toByteArray())));
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleUnknownException();
  }
}

void LogdataController::planAdd(atools::sql::SqlRecord *record, QWidget *parent)
{
  qDebug() << Q_FUNC_INFO;
  planAttachLnmpln(record, mainWindow->routeOpenFileDialog(), parent);
}

void LogdataController::planAttachLnmpln(atools::sql::SqlRecord *record, const QString& filename, QWidget *parent)
{
  try
  {
    // Store a new flight plan as logbook entry attachement - loaded plan can be any supported format
    // Plan is always attached in LNMPLN format
    if(atools::checkFile(Q_FUNC_INFO, filename))
    {
      // Load flight plan in any supported format
      atools::fs::pln::Flightplan flightplan;
      atools::fs::pln::FlightplanIO().load(flightplan, filename);

      if(!flightplan.isEmpty())
        // Store in LNMPLN format
        record->setValue("flightplan", atools::fs::pln::FlightplanIO().saveLnmGz(flightplan));
      else
        record->setNull("flightplan");
    }
    else
      record->setNull("flightplan");
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleUnknownException();
  }
}

void LogdataController::planSaveAs(atools::sql::SqlRecord *record, QWidget *parent)
{
  qDebug() << Q_FUNC_INFO;
  try
  {
    // Load LNMPLN into flight plan
    atools::fs::pln::Flightplan flightplan;
    atools::fs::pln::FlightplanIO().loadLnmGz(flightplan, record->value("flightplan").toByteArray());

    // Build filename
    QString defFilename = buildFilename(record, flightplan, ".lnmpln");
    QString filename = mainWindow->routeSaveFileDialogLnm(defFilename);
    if(!filename.isEmpty())
      atools::fs::pln::FlightplanIO().saveLnm(flightplan, filename);
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleUnknownException();
  }
}

void LogdataController::gpxAttach(atools::sql::SqlRecord *record, QWidget *parent, bool currentTrack)
{
  try
  {
    const AircraftTrack& track = currentTrack ? NavApp::getAircraftTrack() : NavApp::getAircraftTrackLogbook();

    if(!track.isEmpty() || !NavApp::getRouteConst().isEmpty())
      record->setValue("aircraft_trail",
                       FlightplanIO().saveGpxGz(NavApp::getRouteConst().
                                                updatedAltitudes().adjustedToOptions(rf::DEFAULT_OPTS_GPX).
                                                getFlightplan(), track.getLineStrings(), track.getTimestampsMs(),
                                                static_cast<int>(NavApp::getRouteConst().getCruisingAltitudeFeet()))); // blob
    else
      record->setNull("aircraft_trail");

  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleUnknownException();
  }
}

void LogdataController::gpxAdd(atools::sql::SqlRecord *record, QWidget *parent)
{
  qDebug() << Q_FUNC_INFO;
  try
  {
    QString filename = dialog->openFileDialog(
      tr("Open GPX"),
      tr("GPX Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_GPX),
      "Route/Gpx", atools::documentsDir());

    if(!filename.isEmpty())
    {
      QFile file(filename);
      if(file.open(QIODevice::ReadOnly | QIODevice::Text))
      {
        QTextStream stream(&file);
        stream.setCodec("UTF-8");

        // Decompress GPX and save as is into the database
        record->setValue("aircraft_trail", atools::zip::gzipCompress(stream.readAll().toUtf8()));

        file.close();
      }
      else
        atools::gui::ErrorHandler(parent).handleIOError(file, tr("Cannot load file."));
    }
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleUnknownException();
  }
}

QString LogdataController::buildFilename(const atools::sql::SqlRecord *record,
                                         const atools::fs::pln::Flightplan& flightplan, const QString& suffix)
{
  if(!flightplan.isEmpty())
    // Flight plan is valid - extract name from plan object
    return flightplan.getFilenamePattern(OptionData::instance().getFlightplanPattern(), suffix, false /* clean */);
  else if(record != nullptr)
  {
    // No flight plan - extract name from SQL record and values currently set in the GUI
    const Route& route = NavApp::getRouteConst();
    QString type = route.getFlightplan().getFlightplanTypeStr();
    return atools::fs::pln::Flightplan::getFilenamePattern(OptionData::instance().getFlightplanPattern(), type,
                                                           record->valueStr("departure_name"),
                                                           record->valueStr("departure_ident"),
                                                           record->valueStr("destination_name"),
                                                           record->valueStr("destination_ident"), suffix,
                                                           atools::roundToInt(route.getCruisingAltitudeFeet()),
                                                           false /* clean */);
  }

  return tr("Empty Flightplan") + suffix;
}

void LogdataController::gpxSaveAs(atools::sql::SqlRecord *record, QWidget *parent)
{
  qDebug() << Q_FUNC_INFO;
  try
  {
    // Get flight plan for file name building =========
    atools::fs::pln::Flightplan flightplan;
    try
    {
      // Older versions of LNM attached empty and invalid flight plans. Do not show an exception to the user here.
      QString plan = QString(atools::zip::gzipDecompress(record->value("flightplan").toByteArray()));
      atools::fs::pln::FlightplanIO().loadLnmStr(flightplan, plan);
    }
    catch(atools::Exception& e)
    {
      qDebug() << Q_FUNC_INFO << "Error reading flight plan" << e.what();
    }

    QString defFilename = buildFilename(record, flightplan, ".gpx");

    QString filename = dialog->saveFileDialog(
      tr("Save GPX"),
      tr("GPX Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_GPX),
      "lnmpln", "Route/Gpx", atools::documentsDir(), defFilename, false /* confirm overwrite */);

    if(!filename.isEmpty())
    {
      QFile file(filename);
      if(file.open(QIODevice::WriteOnly | QIODevice::Text))
      {
        // Decompress and save track as is ==============
        QTextStream stream(&file);
        stream.setCodec("UTF-8");
        stream << QString(atools::zip::gzipDecompress(record->value("aircraft_trail").toByteArray())).toUtf8();
        file.close();
      }
      else
        atools::gui::ErrorHandler(parent).handleIOError(file, tr("Cannot save file."));
    }
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleUnknownException();
  }
}

void LogdataController::perfAdd(atools::sql::SqlRecord *record, QWidget *parent)
{
  qDebug() << Q_FUNC_INFO;
  perfAttachLnmperf(record, NavApp::getAircraftPerfController()->openFileDialog(), parent);
}

void LogdataController::perfAttachLnmperf(atools::sql::SqlRecord *record, const QString& filename, QWidget *parent)
{
  try
  {
    if(atools::checkFile(Q_FUNC_INFO, filename))
    {
      // Load aircraft performance in any format (INI or XML) ===================
      atools::fs::perf::AircraftPerf perf;
      perf.load(filename);

      // Save in new XML format ============
      record->setValue("aircraft_perf", perf.saveXmlGz());
    }
    else
      record->setNull("aircraft_perf");
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleUnknownException();
  }
}

void LogdataController::perfOpen(atools::sql::SqlRecord *record, QWidget *parent)
{
  qDebug() << Q_FUNC_INFO;

  try
  {
    NavApp::getAircraftPerfController()->loadStr(QString(atools::zip::gzipDecompress(record->value("aircraft_perf").toByteArray())));
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
  }
}

void LogdataController::perfSaveAs(atools::sql::SqlRecord *record, QWidget *parent)
{
  qDebug() << Q_FUNC_INFO;
  try
  {
    NavApp::getAircraftPerfController()->saveAsStr(QString(atools::zip::gzipDecompress(record->value("aircraft_perf").toByteArray())));
  }
  catch(atools::Exception& e)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(parent).handleException(e);
  }
  catch(...)
  {
    NavApp::closeSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
  }
}
