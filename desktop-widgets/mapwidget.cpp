// SPDX-License-Identifier: GPL-2.0
#include <QQmlContext>
#include <QDebug>
#include <QQuickItem>
#include <QModelIndex>

#include "mapwidget.h"
#include "core/dive.h"
#include "core/divesite.h"
#include "mobile-widgets/qmlmapwidgethelper.h"
#include "qt-models/maplocationmodel.h"
#include "mainwindow.h"
#include "divelistview.h"

static bool skipReload = false;

MapWidget *MapWidget::m_instance = NULL;

MapWidget::MapWidget(QWidget *parent) : QQuickWidget(parent)
{
	qmlRegisterType<MapWidgetHelper>("org.subsurfacedivelog.mobile", 1, 0, "MapWidgetHelper");
	qmlRegisterType<MapLocationModel>("org.subsurfacedivelog.mobile", 1, 0, "MapLocationModel");
	qmlRegisterType<MapLocation>("org.subsurfacedivelog.mobile", 1, 0, "MapLocation");

	setSource(QUrl(QStringLiteral("qrc:/MapWidget.qml")));
	setResizeMode(QQuickWidget::SizeRootObjectToView);

	m_rootItem = qobject_cast<QQuickItem *>(rootObject());
	m_mapHelper = rootObject()->findChild<MapWidgetHelper *>();
	connect(m_mapHelper, SIGNAL(selectedDivesChanged(QList<int>)),
	        this, SLOT(selectedDivesChanged(QList<int>)));
}

void MapWidget::centerOnDiveSite(struct dive_site *ds)
{
	m_mapHelper->centerOnDiveSite(ds);
}

void MapWidget::centerOnIndex(const QModelIndex& idx)
{
	struct dive_site *ds = get_dive_site_by_uuid(idx.model()->index(idx.row(), 0).data().toInt());
	if (!ds || !dive_site_has_gps_location(ds))
		centerOnDiveSite(&displayed_dive_site);
	else
		centerOnDiveSite(ds);
}

void MapWidget::repopulateLabels()
{
	m_mapHelper->reloadMapLocations();
}

void MapWidget::reload()
{
	if (!skipReload)
		m_mapHelper->reloadMapLocations();
}

void MapWidget::endGetDiveCoordinates()
{
	// TODO;
}

void MapWidget::prepareForGetDiveCoordinates()
{
	// TODO;
}

void MapWidget::selectedDivesChanged(QList<int> list)
{
	qDebug() << "onSelectedDivesChanged:" << list.size();
	skipReload = true;
	MainWindow::instance()->dive_list()->unselectDives();
	if (!list.empty())
		MainWindow::instance()->dive_list()->selectDives(list);
	skipReload = false;
}

MapWidget::~MapWidget()
{
	m_instance = NULL;
}

MapWidget *MapWidget::instance()
{
	if (m_instance == NULL)
		m_instance = new MapWidget();
	return m_instance;
}
