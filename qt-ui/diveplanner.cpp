#include "diveplanner.h"
#include "graphicsview-common.h"
#include "models.h"
#include "modeldelegates.h"
#include "ui_diveplanner.h"
#include "mainwindow.h"
#include "tableview.h"

#include "../dive.h"
#include "../divelist.h"
#include "../planner.h"

#include <QMouseEvent>
#include <QDebug>
#include <QGraphicsSceneMouseEvent>
#include <QMessageBox>
#include <QStringListModel>
#include <QListView>
#include <QModelIndex>
#include <QSettings>
#include <QTableView>

#define TIME_INITIAL_MAX 30

#define MAX_DEEPNESS 150
#define MIN_DEEPNESS 40

QStringListModel *airTypes(){
	static QStringListModel *self = new QStringListModel(QStringList()
		<< QObject::tr("AIR")
		<< QObject::tr("EAN32")
		<< QObject::tr("EAN36"));
	return self;
}

QString strForAir(const divedatapoint& p){
	return p.o2 == 209 ? QObject::tr("AIR")
		: p.o2 == 320 ? QObject::tr("EAN32")
		: p.o2 == 360 ? QObject::tr("EAN36")
		: QObject::tr("Choose Gas");
}

static DivePlannerPointsModel *plannerModel = DivePlannerPointsModel::instance();

DivePlannerGraphics::DivePlannerGraphics(QWidget* parent): QGraphicsView(parent), activeDraggedHandler(0)
{
	fill_profile_color();
	setBackgroundBrush(profile_color[BACKGROUND].at(0));
	setMouseTracking(true);
	setScene(new QGraphicsScene());
	scene()->setSceneRect(0,0,1920,1080);

	verticalLine = new QGraphicsLineItem(
		fromPercent(0, Qt::Horizontal),
		fromPercent(0, Qt::Vertical),
		fromPercent(0, Qt::Horizontal),
		fromPercent(100, Qt::Vertical)
	);

	verticalLine->setPen(QPen(Qt::DotLine));
	scene()->addItem(verticalLine);

	horizontalLine = new QGraphicsLineItem(
		fromPercent(0, Qt::Horizontal),
		fromPercent(0, Qt::Vertical),
		fromPercent(100, Qt::Horizontal),
		fromPercent(0, Qt::Vertical)
	);
	horizontalLine->setPen(QPen(Qt::DotLine));
	scene()->addItem(horizontalLine);

	timeLine = new Ruler();
	timeLine->setMinimum(0);
	timeLine->setMaximum(TIME_INITIAL_MAX);
	timeLine->setTickInterval(10);
	timeLine->setLine(
		fromPercent(10, Qt::Horizontal),
		fromPercent(90, Qt::Vertical),
		fromPercent(90, Qt::Horizontal),
		fromPercent(90, Qt::Vertical)
	);
	timeLine->setOrientation(Qt::Horizontal);
	timeLine->setTickSize(fromPercent(1, Qt::Vertical));
	timeLine->setColor(profile_color[TIME_GRID].at(0));
	timeLine->updateTicks();
	scene()->addItem(timeLine);

	depthLine = new Ruler();
	depthLine->setMinimum(0);
	depthLine->setMaximum(40);
	depthLine->setTickInterval(10);
	depthLine->setLine(
		fromPercent(10, Qt::Horizontal),
		fromPercent(10, Qt::Vertical),
		fromPercent(10, Qt::Horizontal),
		fromPercent(90, Qt::Vertical)
	);
	depthLine->setOrientation(Qt::Vertical);
	depthLine->setTickSize(fromPercent(1, Qt::Horizontal));
	depthLine->setColor(profile_color[DEPTH_GRID].at(0));
	depthLine->updateTicks();
	scene()->addItem(depthLine);

	timeString = new QGraphicsSimpleTextItem();
	timeString->setFlag(QGraphicsItem::ItemIgnoresTransformations);
	timeString->setBrush(profile_color[TIME_TEXT].at(0));
	scene()->addItem(timeString);

	depthString = new QGraphicsSimpleTextItem();
	depthString->setFlag(QGraphicsItem::ItemIgnoresTransformations);
	depthString->setBrush(profile_color[SAMPLE_DEEP].at(0));
	scene()->addItem(depthString);

	diveBg = new QGraphicsPolygonItem();
	diveBg->setPen(QPen(QBrush(),0));
	scene()->addItem(diveBg);

#define ADDBTN(obj, icon, text, horizontal, vertical, tooltip, slot) \
	obj = new Button(); \
	obj->setPixmap(QPixmap(icon)); \
	obj->setPos(fromPercent(horizontal, Qt::Horizontal), fromPercent(vertical, Qt::Vertical)); \
	obj->setToolTip(tooltip); \
	scene()->addItem(obj); \
	connect(obj, SIGNAL(clicked()), this, SLOT(slot));

	ADDBTN(plusDepth, ":plus",   ""  , 5,  5, tr("Increase maximum depth by 10m"), increaseDepth());
	ADDBTN(plusTime,  ":plus",   ""  , 95, 95, tr("Increase minimum time by 10m"), increaseTime());
	ADDBTN(lessDepth, ":minimum",""  , 2,  5, tr("Decreases maximum depth by 10m"), decreaseDepth());
	ADDBTN(lessTime,  ":minimum",""  , 92, 95, tr("Decreases minimum time by 10m"), decreaseTime());
#undef ADDBTN
	minMinutes = TIME_INITIAL_MAX;

	QAction *action = NULL;

#define ADD_ACTION( SHORTCUT, Slot ) \
	action = new QAction(this); \
	action->setShortcut( SHORTCUT ); \
	action->setShortcutContext(Qt::WindowShortcut); \
	addAction(action); \
	connect(action, SIGNAL(triggered(bool)), this, SLOT( Slot ))

	ADD_ACTION(Qt::Key_Escape, keyEscAction());
	ADD_ACTION(Qt::Key_Delete, keyDeleteAction());
	ADD_ACTION(Qt::Key_Up, keyUpAction());
	ADD_ACTION(Qt::Key_Down, keyDownAction());
	ADD_ACTION(Qt::Key_Left, keyLeftAction());
	ADD_ACTION(Qt::Key_Right, keyRightAction());
#undef ADD_ACTION

	// Prepare the stuff for the gas-choices.
	gasListView = new QListView();
	gasListView->setWindowFlags(Qt::Popup);
	gasListView->setModel(airTypes());
	gasListView->hide();

	connect(gasListView, SIGNAL(activated(QModelIndex)), this, SLOT(selectGas(QModelIndex)));
	connect(plannerModel, SIGNAL(dataChanged(QModelIndex,QModelIndex)), this, SLOT(createDecoStops()));

	connect(plannerModel, SIGNAL(rowsInserted(const QModelIndex&,int,int)),
			this, SLOT(pointInserted(const QModelIndex&, int, int)));
	connect(plannerModel, SIGNAL(rowsRemoved(const QModelIndex&, int, int)),
			this, SLOT(pointsRemoved(const QModelIndex&, int, int)));
	setRenderHint(QPainter::Antialiasing);
}

void DivePlannerGraphics::pointInserted(const QModelIndex& parent, int start , int end)
{
	DiveHandler *item = new DiveHandler ();
	scene()->addItem(item);
	handles << item;

	Button *gasChooseBtn = new Button();
	scene()->addItem(gasChooseBtn);
	gasChooseBtn->setZValue(10);
	connect(gasChooseBtn, SIGNAL(clicked()), this, SLOT(prepareSelectGas()));

	gases << gasChooseBtn;
	createDecoStops();
}

void DivePlannerGraphics::keyDownAction()
{
	if(scene()->selectedItems().count()){
		Q_FOREACH(QGraphicsItem *i, scene()->selectedItems()){
			if (DiveHandler *handler = qgraphicsitem_cast<DiveHandler*>(i)){
				int row = handles.indexOf(handler);
				divedatapoint dp = plannerModel->at(row);
				if (dp.depth / 1000 >= depthLine->maximum())
					continue;

				dp.depth += 1000;
				plannerModel->editStop(row, dp);
			}
		}
	}
}

void DivePlannerGraphics::keyUpAction()
{
	Q_FOREACH(QGraphicsItem *i, scene()->selectedItems()){
		if (DiveHandler *handler = qgraphicsitem_cast<DiveHandler*>(i)){
			int row = handles.indexOf(handler);
			divedatapoint dp = plannerModel->at(row);

			if (dp.depth / 1000 <= 0)
				continue;

			dp.depth -= 1000;
			plannerModel->editStop(row, dp);
		}
	}
	createDecoStops();
}

void DivePlannerGraphics::keyLeftAction()
{
	Q_FOREACH(QGraphicsItem *i, scene()->selectedItems()){
		if (DiveHandler *handler = qgraphicsitem_cast<DiveHandler*>(i)){
			int row = handles.indexOf(handler);
			divedatapoint dp = plannerModel->at(row);

			if (dp.time / 60 <= 0)
				continue;

			// don't overlap positions.
			// maybe this is a good place for a 'goto'?
			double xpos = timeLine->posAtValue((dp.time - 60) / 60);
			bool nextStep = false;
			Q_FOREACH(DiveHandler *h, handles){
				if (h->pos().x() == xpos){
					nextStep = true;
					break;
				}
			}
			if(nextStep)
				continue;

			dp.time -= 60;
			plannerModel->editStop(row, dp);
		}
	}
}

void DivePlannerGraphics::keyRightAction()
{
	Q_FOREACH(QGraphicsItem *i, scene()->selectedItems()){
		if (DiveHandler *handler = qgraphicsitem_cast<DiveHandler*>(i)){
			int row = handles.indexOf(handler);
			divedatapoint dp = plannerModel->at(row);
			if (dp.time / 60 >= timeLine->maximum())
				continue;

			// don't overlap positions.
			// maybe this is a good place for a 'goto'?
			double xpos = timeLine->posAtValue((dp.time + 60) / 60);
			bool nextStep = false;
			Q_FOREACH(DiveHandler *h, handles){
				if (h->pos().x() == xpos){
					nextStep = true;
					break;
				}
			}
			if(nextStep)
				continue;

			dp.time += 60;
			plannerModel->editStop(row, dp);
		}
	}
}

void DivePlannerGraphics::keyDeleteAction()
{
	int selCount = scene()->selectedItems().count();
	if(selCount){
		QVector<int> selectedIndexes;
		Q_FOREACH(QGraphicsItem *i, scene()->selectedItems()){
			if (DiveHandler *handler = qgraphicsitem_cast<DiveHandler*>(i)){
				selectedIndexes.push_back(handles.indexOf(handler));
			}
		}
		plannerModel->removeSelectedPoints(selectedIndexes);
	}
}

void DivePlannerGraphics::pointsRemoved(const QModelIndex& , int start, int end)
{	// start and end are inclusive.
	int num = (end - start) + 1;
	for(int i = num; i != 0; i--){
		delete handles.back();
		handles.pop_back();
		delete gases.back();
		gases.pop_back();
	}
	scene()->clearSelection();
	createDecoStops();
}

bool intLessThan(int a, int b){
	return a <= b;
}
void DivePlannerPointsModel::removeSelectedPoints(const QVector< int >& rows)
{
	int firstRow = rowCount() - rows.count();
	QVector<int> v2 = rows;
	std::sort(v2.begin(), v2.end(), intLessThan);
	beginRemoveRows(QModelIndex(), firstRow, rowCount()-1);
	for(int i = v2.count()-1; i >= 0; i--){
		divepoints.remove(v2[i]);
	}
	endRemoveRows();
}

void DivePlannerGraphics::keyEscAction()
{
	if (scene()->selectedItems().count()){
		scene()->clearSelection();
		return;
	}
	plannerModel->cancelPlan();
}

qreal DivePlannerGraphics::fromPercent(qreal percent, Qt::Orientation orientation)
{
	qreal total = orientation == Qt::Horizontal ? sceneRect().width() : sceneRect().height();
	qreal result = (total * percent) / 100;
	return result;
}

void DivePlannerGraphics::increaseDepth()
{
	if (depthLine->maximum() + 10 > MAX_DEEPNESS)
		return;
	depthLine->setMaximum( depthLine->maximum() + 10);
	depthLine->updateTicks();
	createDecoStops();
}

void DivePlannerGraphics::increaseTime()
{
	minMinutes += 10;
	timeLine->setMaximum( minMinutes );
	timeLine->updateTicks();
	createDecoStops();
}

void DivePlannerGraphics::decreaseDepth()
{
	if (depthLine->maximum() - 10 < MIN_DEEPNESS)
		return;

	Q_FOREACH(DiveHandler *d, handles){
		if (depthLine->valueAt(d->pos()) > depthLine->maximum() - 10){
			QMessageBox::warning(mainWindow(),
				tr("Handler Position Error"),
				tr("One or more of your stops will be lost with this operations, \n"
					"Please, remove them first."));
			return;
		}
	}
	depthLine->setMaximum(depthLine->maximum() - 10);
	depthLine->updateTicks();
	createDecoStops();
}

void DivePlannerGraphics::decreaseTime()
{
	if (timeLine->maximum() -10 < TIME_INITIAL_MAX){
		return;
	}
	if (timeLine->maximum() - 10 < dpMaxTime){
		qDebug() << timeLine->maximum() << dpMaxTime;
		return;
	}
	minMinutes -= 10;
	timeLine->setMaximum(timeLine->maximum() -10);
	timeLine->updateTicks();
	createDecoStops();
}

void DivePlannerGraphics::mouseDoubleClickEvent(QMouseEvent* event)
{
	QPointF mappedPos = mapToScene(event->pos());
	if (isPointOutOfBoundaries(mappedPos))
		return;

	int minutes = rint(timeLine->valueAt(mappedPos));
	int meters = rint(depthLine->valueAt(mappedPos));
	plannerModel->addStop(meters * 1000, minutes * 60, tr("Air"), 0);
}

void DivePlannerGraphics::prepareSelectGas()
{
	currentGasChoice = static_cast<Button*>(sender());
	QPoint c = QCursor::pos();
	gasListView->setGeometry(c.x(), c.y(), 150, 100);
	gasListView->show();
}

void DivePlannerGraphics::selectGas(const QModelIndex& index)
{
	QString gasSelected = gasListView->model()->data(index, Qt::DisplayRole).toString();
	int idx = gases.indexOf(currentGasChoice);
	plannerModel->setData(plannerModel->index(idx, DivePlannerPointsModel::GAS), gasSelected);
	gasListView->hide();
}

void DivePlannerGraphics::createDecoStops()
{
	qDeleteAll(lines);
	lines.clear();

	plannerModel->createTemporaryPlan();
	struct diveplan diveplan = plannerModel->getDiveplan();
	struct divedatapoint *dp = diveplan.dp;
	while(dp->next){
		dp = dp->next;
	}

	if (timeLine->maximum() < dp->time / 60.0 + 5 || dp->time / 60.0 + 15 < timeLine->maximum()) {
		double newMax = fmax(dp->time / 60.0 + 5, minMinutes);
		timeLine->setMaximum(newMax);
		timeLine->updateTicks();
	}

	// Re-position the user generated dive handlers
	for(int i = 0; i < plannerModel->rowCount(); i++){
		divedatapoint dp = plannerModel->at(i);
		DiveHandler *h = handles.at(i);
		h->setPos(timeLine->posAtValue(dp.time / 60), depthLine->posAtValue(dp.depth / 1000));
		QPointF p1 = (i == 0) ? QPointF(timeLine->posAtValue(0), depthLine->posAtValue(0)) : handles[i-1]->pos();
		QPointF p2 = handles[i]->pos();
		QLineF line(p1, p2);
		QPointF pos = line.pointAt(0.5);
		gases[i]->setPos(pos);
		gases[i]->setText( strForAir(dp));
	}

	// (re-) create the profile with different colors for segments that were
	// entered vs. segments that were calculated
	double lastx = timeLine->posAtValue(0);
	double lasty = depthLine->posAtValue(0);

	QPolygonF poly;
	poly.append(QPointF(lastx, lasty));

	for (dp = diveplan.dp; dp != NULL; dp = dp->next) {
		double xpos = timeLine->posAtValue(dp->time / 60.0);
		double ypos = depthLine->posAtValue(dp->depth / 1000.0);
		if(!dp->entered){
			QGraphicsLineItem *item = new QGraphicsLineItem(lastx, lasty, xpos, ypos);
			item->setPen(QPen(QBrush(Qt::red),0));
			scene()->addItem(item);
			lines << item;
		}
		lastx = xpos;
		lasty = ypos;
		poly.append(QPointF(lastx, lasty));
	}

	diveBg->setPolygon(poly);
	QRectF b = poly.boundingRect();
	QLinearGradient pat(
		b.x(),
		b.y(),
		b.x(),
		b.height() + b.y()
	);

	pat.setColorAt(1, profile_color[DEPTH_BOTTOM].first());
	pat.setColorAt(0, profile_color[DEPTH_TOP].first());
	diveBg->setBrush(pat);

	plannerModel->deleteTemporaryPlan();
}

void DivePlannerGraphics::resizeEvent(QResizeEvent* event)
{
	QGraphicsView::resizeEvent(event);
	fitInView(sceneRect(), Qt::IgnoreAspectRatio);
}

void DivePlannerGraphics::showEvent(QShowEvent* event)
{
	QGraphicsView::showEvent(event);
	fitInView(sceneRect(), Qt::IgnoreAspectRatio);
}

void DivePlannerGraphics::mouseMoveEvent(QMouseEvent* event)
{
	QPointF mappedPos = mapToScene(event->pos());
	if (isPointOutOfBoundaries(mappedPos))
		return;

	verticalLine->setPos(mappedPos.x(), fromPercent(0, Qt::Vertical));
	horizontalLine->setPos(fromPercent(0, Qt::Horizontal), mappedPos.y());

	depthString->setText(QString::number(rint(depthLine->valueAt(mappedPos))) + "m" );
	depthString->setPos(fromPercent(5, Qt::Horizontal), mappedPos.y());
	timeString->setText(QString::number(rint(timeLine->valueAt(mappedPos))) + "min");
	timeString->setPos(mappedPos.x()+1, fromPercent(95, Qt::Vertical));

	// calculate the correct color for the depthString.
	// QGradient doesn't returns it's interpolation, meh.
	double percent = depthLine->percentAt(mappedPos);
	QColor& startColor = profile_color[SAMPLE_SHALLOW].first();
	QColor& endColor = profile_color[SAMPLE_DEEP].first();
	short redDelta = (endColor.red() - startColor.red()) * percent + startColor.red();
	short greenDelta = (endColor.green() - startColor.green()) * percent + startColor.green();
	short blueDelta = (endColor.blue() - startColor.blue()) * percent + startColor.blue();
	depthString->setBrush( QColor(redDelta, greenDelta, blueDelta));

	if (activeDraggedHandler)
		moveActiveHandler(mappedPos);
	if (!handles.count())
		return;

	if (handles.last()->x() > mappedPos.x()) {
		verticalLine->setPen(QPen(QBrush(Qt::red), 0, Qt::SolidLine));
		horizontalLine->setPen(QPen(QBrush(Qt::red), 0, Qt::SolidLine));
	} else {
		verticalLine->setPen(QPen(Qt::DotLine));
		horizontalLine->setPen(QPen(Qt::DotLine));
	}
}

void DivePlannerGraphics::moveActiveHandler(const QPointF& pos)
{
	double xpos = timeLine->posAtValue(rint(timeLine->valueAt(pos)));
	double ypos = depthLine->posAtValue(rint(depthLine->valueAt(pos)));
	activeDraggedHandler->setPos(QPointF(xpos, ypos));
	qDeleteAll(lines);
	lines.clear();
}

bool DivePlannerGraphics::isPointOutOfBoundaries(const QPointF& point)
{
	double xpos = timeLine->valueAt(point);
	double ypos = depthLine->valueAt(point);

	if (xpos > timeLine->maximum() ||
	    xpos < timeLine->minimum() ||
	    ypos > depthLine->maximum() ||
	    ypos < depthLine->minimum())
	{
		return true;
	}
	return false;
}

void DivePlannerGraphics::mousePressEvent(QMouseEvent* event)
{
	if (event->modifiers()){
		QGraphicsView::mousePressEvent(event);
		return;
	}

    QPointF mappedPos = mapToScene(event->pos());
	Q_FOREACH(QGraphicsItem *item, scene()->items(mappedPos, Qt::IntersectsItemBoundingRect, Qt::AscendingOrder, transform())){
		if (DiveHandler *h = qgraphicsitem_cast<DiveHandler*>(item)) {
			activeDraggedHandler = h;
			activeDraggedHandler->setBrush(Qt::red);
			originalHandlerPos = activeDraggedHandler->pos();
		}
	}
	QGraphicsView::mousePressEvent(event);
}

void DivePlannerGraphics::mouseReleaseEvent(QMouseEvent* event)
{
	if (activeDraggedHandler) {
		QPointF mappedPos = mapToScene(event->pos());
		int minutes = rint(timeLine->valueAt(mappedPos));
		int meters = rint(depthLine->valueAt(mappedPos));
		double xpos = timeLine->posAtValue(minutes);
		double ypos = depthLine->posAtValue(meters);
		Q_FOREACH(DiveHandler* handler, handles){
			if (xpos == handler->pos().x() && handler != activeDraggedHandler){
				qDebug() << "There's already an point at that place.";
				//TODO: Move this later to a KMessageWidget.
				activeDraggedHandler->setPos(originalHandlerPos);
				activeDraggedHandler = NULL;
				return;
			}
		}

		int pos = handles.indexOf(activeDraggedHandler);
		divedatapoint data = plannerModel->at(pos);

		data.depth = rint(depthLine->valueAt(mappedPos)) * 1000;
		data.time = rint(timeLine->valueAt(mappedPos)) * 60;

		plannerModel->editStop(pos, data);

		activeDraggedHandler->setBrush(QBrush(Qt::white));
		activeDraggedHandler->setPos(QPointF(xpos, ypos));

		createDecoStops();
		activeDraggedHandler = 0;
	}
}

DiveHandler::DiveHandler(): QGraphicsEllipseItem()
{
	setRect(-5,-5,10,10);
	setFlag(QGraphicsItem::ItemIgnoresTransformations);
	setFlag(QGraphicsItem::ItemIsSelectable);
	setBrush(Qt::white);
	setZValue(2);
}

void DiveHandler::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
	if (event->modifiers().testFlag(Qt::ControlModifier)){
		setSelected(true);
	}
	// mousePressEvent 'grabs' the mouse and keyboard, annoying.
	ungrabMouse();

	/* hack. Sometimes the keyboard is grabbed, sometime it's not,
	so, let's force a grab and release, to get rid of a warning. */
	grabKeyboard();
	ungrabKeyboard();
}

void Ruler::setMaximum(double maximum)
{
	maxText->setText(QString::number(maximum));
	if(orientation == Qt::Horizontal)
		maxText->setPos( line().x2(), line().y2() -5 );
	else
		maxText->setPos( line().x1() - 50, line().y2());
	max = maximum;
}

void Ruler::setMinimum(double minimum)
{
	minText->setText(QString::number(minimum));
	if(orientation == Qt::Horizontal)
		minText->setPos( line().x1(), line().y2() -5 );
	else
		minText->setPos( line().x1() - 50, line().y1());
	min = minimum;
}

Ruler::Ruler() : orientation(Qt::Horizontal),
minText(new QGraphicsSimpleTextItem(this)),
maxText(new QGraphicsSimpleTextItem(this))
{
	minText->setFlag(QGraphicsItem::ItemIgnoresTransformations);
	maxText->setFlag(QGraphicsItem::ItemIgnoresTransformations);
}

void Ruler::setOrientation(Qt::Orientation o)
{
	orientation = o;
	// position the elements on the screen.
	setMinimum(minimum());
	setMaximum(maximum());
}

void Ruler::updateTicks()
{
	qDeleteAll(ticks);
	ticks.clear();
	QLineF m = line();
	QGraphicsLineItem *item = NULL;

	if (orientation == Qt::Horizontal) {
		double steps = (max - min) / interval;
		double stepSize = (m.x2() - m.x1()) / steps;
		qreal pos;
		for (pos = m.x1(); pos < m.x2(); pos += stepSize) {
			item = new QGraphicsLineItem(pos, m.y1(), pos, m.y1() + tickSize, this);
			item->setPen(pen());
			ticks.push_back(item);
		}
		item = new QGraphicsLineItem(pos, m.y1(), pos, m.y1() + tickSize, this);
		item->setPen(pen());
		ticks.push_back(item);
	} else {
		double steps = (max - min) / interval;
		double stepSize = (m.y2() - m.y1()) / steps;
		qreal pos;
		for (pos = m.y1(); pos < m.y2(); pos += stepSize) {
			item = new QGraphicsLineItem(m.x1(), pos, m.x1() - tickSize, pos, this);
			item->setPen(pen());
			ticks.push_back(item);
		}
		item = new QGraphicsLineItem(m.x1(), pos, m.x1() - tickSize, pos, this);
		item->setPen(pen());
		ticks.push_back(item);
	}
}

void Ruler::setTickSize(qreal size)
{
	tickSize = size;
}


void Ruler::setTickInterval(double i)
{
	interval = i;
}

qreal Ruler::valueAt(const QPointF& p)
{
	QLineF m = line();
	double retValue =  orientation == Qt::Horizontal ?
				max * (p.x() - m.x1()) / (m.x2() - m.x1()) :
				max * (p.y() - m.y1()) / (m.y2() - m.y1());
	return retValue;
}

qreal Ruler::posAtValue(qreal value)
{
	QLineF m = line();
	double size = max - min;
	double percent = value / size;
	double realSize = orientation == Qt::Horizontal ?
				m.x2() - m.x1() :
				m.y2() - m.y1();
	double retValue = realSize * percent;
	retValue =  (orientation == Qt::Horizontal) ?
				retValue + m.x1() :
				retValue + m.y1();
	return retValue;
}

qreal Ruler::percentAt(const QPointF& p)
{
	qreal value = valueAt(p);
	double size = max - min;
	double percent = value / size;
	return percent;
}


double Ruler::maximum() const
{
	return max;
}

double Ruler::minimum() const
{
	return min;
}

void Ruler::setColor(const QColor& color)
{
	setPen(QPen(color));
}

Button::Button(QObject* parent): QObject(parent), QGraphicsRectItem()
{
	icon = new QGraphicsPixmapItem(this);
	text = new QGraphicsSimpleTextItem(this);
	icon->setPos(0,0);
	text->setPos(0,0);
	setFlag(ItemIgnoresTransformations);
	setPen(QPen(QBrush(), 0));
}

void Button::setPixmap(const QPixmap& pixmap)
{
	icon->setPixmap(pixmap.scaled(20,20, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	if(pixmap.isNull()){
		icon->hide();
	}else{
		icon->show();
	}
	setRect(childrenBoundingRect());
}

void Button::setText(const QString& t)
{
	text->setText(t);
	if(icon->pixmap().isNull()){
		icon->hide();
		text->setPos(0,0);
	}else{
		icon->show();
		text->setPos(22,0);
	}
	setRect(childrenBoundingRect());
}

void Button::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
	event->ignore();
	emit clicked();
}

DivePlannerWidget::DivePlannerWidget(QWidget* parent, Qt::WindowFlags f): QWidget(parent, f), ui(new Ui::DivePlanner())
{
	ui->setupUi(this);
	ui->tableWidget->setTitle(tr("Dive Planner Points"));
	ui->tableWidget->setModel(DivePlannerPointsModel::instance());
	ui->tableWidget->view()->setItemDelegateForColumn(DivePlannerPointsModel::GAS, new AirTypesDelegate(this));

	connect(ui->tableWidget, SIGNAL(addButtonClicked()), DivePlannerPointsModel::instance(), SLOT(addStop()));
	connect(ui->startTime, SIGNAL(timeChanged(QTime)), this, SLOT(startTimeChanged(QTime)));
	connect(ui->ATMPressure, SIGNAL(textChanged(QString)), this, SLOT(atmPressureChanged(QString)));
	connect(ui->bottomSAC, SIGNAL(textChanged(QString)), this, SLOT(bottomSacChanged(QString)));
	connect(ui->decoStopSAC, SIGNAL(textChanged(QString)), this, SLOT(decoSacChanged(QString)));
	connect(ui->highGF, SIGNAL(textChanged(QString)), this, SLOT(gfhighChanged(QString)));
	connect(ui->lowGF, SIGNAL(textChanged(QString)), this, SLOT(gflowChanged(QString)));
	connect(ui->highGF, SIGNAL(textChanged(QString)), this, SLOT(gfhighChanged(QString)));
	connect(ui->lastStop, SIGNAL(toggled(bool)), this, SLOT(lastStopChanged(bool)));

	// Creating the plan
	connect(ui->buttonBox, SIGNAL(accepted()), plannerModel, SLOT(createPlan()));
	connect(ui->buttonBox, SIGNAL(rejected()), plannerModel, SLOT(cancelPlan()));
	connect(plannerModel, SIGNAL(planCreated()), mainWindow(), SLOT(showProfile()));
	connect(plannerModel, SIGNAL(planCanceled()), mainWindow(), SLOT(showProfile()));

	/* set defaults. */
	ui->startTime->setTime( QTime(1, 0) );
	ui->ATMPressure->setText( "1013" );
	ui->bottomSAC->setText("20");
	ui->decoStopSAC->setText("17");
	ui->lowGF->setText("30");
	ui->highGF->setText("75");
}

void DivePlannerWidget::startTimeChanged(const QTime& time)
{
	plannerModel->setStartTime(time);
}

void DivePlannerWidget::atmPressureChanged(const QString& pressure)
{
	plannerModel->setSurfacePressure(pressure.toInt());
}

void DivePlannerWidget::bottomSacChanged(const QString& bottomSac)
{
	plannerModel->setBottomSac(bottomSac.toInt());
}

void DivePlannerWidget::decoSacChanged(const QString& decosac)
{
	plannerModel->setDecoSac(decosac.toInt());
}

void DivePlannerWidget::gfhighChanged(const QString& gfhigh)
{
	plannerModel->setGFHigh(gfhigh.toShort());
}

void DivePlannerWidget::gflowChanged(const QString& gflow)
{
	plannerModel->setGFLow(gflow.toShort());
}

void DivePlannerWidget::lastStopChanged(bool checked)
{
	plannerModel->setLastStop6m(checked);
}

int DivePlannerPointsModel::columnCount(const QModelIndex& parent) const
{
	return COLUMNS;
}

QVariant DivePlannerPointsModel::data(const QModelIndex& index, int role) const
{
	if(role == Qt::DisplayRole){
		divedatapoint p = divepoints.at(index.row());
		switch(index.column()){
			case CCSETPOINT: return p.po2;
			case DEPTH: return p.depth / 1000;
			case DURATION: return p.time / 60;
			case GAS: return strForAir(p);
		}
	}
	else if (role == Qt::DecorationRole){
		switch(index.column()){
			case REMOVE : return QIcon(":trash");
		}
	}
	else if (role == Qt::FontRole){
		return  defaultModelFont();
	}
	return QVariant();
}

bool DivePlannerPointsModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
	if(role == Qt::EditRole){
		divedatapoint& p = divepoints[index.row()];
		switch(index.column()){
			case DEPTH: p.depth = value.toInt() * 1000; break;
			case DURATION: p.time = value.toInt() * 60; break;
			case CCSETPOINT:{
				int po2 = 0;
				QByteArray gasv = value.toByteArray();
				if (validate_po2(gasv.data(), &po2))
					p.po2 = po2;
			} break;
			case GAS: {
				int o2 = 0;
				int he = 0;
				QByteArray gasv = value.toByteArray();
				if (validate_gas(gasv.data(), &o2, &he)) {
					p.o2 = o2;
					p.he = he;
				}break;
			}
		}
		editStop(index.row(), p);
	}
	return QAbstractItemModel::setData(index, value, role);
}

QVariant DivePlannerPointsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
	if (role == Qt::DisplayRole && orientation == Qt::Horizontal){
		switch(section){
			case DEPTH: return tr("Final Depth");
			case DURATION: return tr("Duration");
			case GAS: return tr("Used Gas");
			case CCSETPOINT: return tr("CC Set Point");
		}
	}
	else if (role == Qt::FontRole){
		return  defaultModelFont();
	}
	return QVariant();
}

Qt::ItemFlags DivePlannerPointsModel::flags(const QModelIndex& index) const
{
    return QAbstractItemModel::flags(index) | Qt::ItemIsEditable;
}

int DivePlannerPointsModel::rowCount(const QModelIndex& parent) const
{
	return divepoints.count();
}

DivePlannerPointsModel::DivePlannerPointsModel(QObject* parent): QAbstractTableModel(parent)
{
}

DivePlannerPointsModel* DivePlannerPointsModel::instance()
{
	static DivePlannerPointsModel* self = new DivePlannerPointsModel();
	return self;
}

void DivePlannerPointsModel::createPlan()
{

}

void DivePlannerPointsModel::setBottomSac(int sac)
{
	diveplan.bottomsac = sac;
	emit dataChanged(createIndex(0, 0), createIndex(rowCount()-1, COLUMNS-1));
}

void DivePlannerPointsModel::setDecoSac(int sac)
{
	diveplan.decosac = sac;
	emit dataChanged(createIndex(0, 0), createIndex(rowCount()-1, COLUMNS-1));
}

void DivePlannerPointsModel::setGFHigh(short int gfhigh)
{
	diveplan.gfhigh = gfhigh;
	emit dataChanged(createIndex(0, 0), createIndex(rowCount()-1, COLUMNS-1));
}

void DivePlannerPointsModel::setGFLow(short int ghflow)
{
	diveplan.gflow = ghflow;
	emit dataChanged(createIndex(0, 0), createIndex(rowCount()-1, COLUMNS-1));
}

void DivePlannerPointsModel::setSurfacePressure(int pressure)
{
	diveplan.surface_pressure = pressure;
	emit dataChanged(createIndex(0, 0), createIndex(rowCount()-1, COLUMNS-1));
}

void DivePlannerPointsModel::setLastStop6m(bool value)
{
}

void DivePlannerPointsModel::setStartTime(const QTime& t)
{
	diveplan.when = t.msec();
	emit dataChanged(createIndex(0, 0), createIndex(rowCount()-1, COLUMNS-1));
}

bool divePointsLessThan(const divedatapoint& p1, const divedatapoint& p2){
	return p1.time <= p2.time;
}
int DivePlannerPointsModel::addStop(int meters, int minutes, const QString& gas, int ccpoint)
{
	int row = divepoints.count();
	if(meters == 0 && minutes == 0){
		if(row == 0){
			meters = 10000;
			minutes = 600;
		}
		else{
			divedatapoint p = at(row-1);
			meters = p.depth;
			minutes = p.time + 600;
		}
	}

	// check if there's already a new stop before this one:
	for(int i = 0; i < divepoints.count(); i++){
		const divedatapoint& dp = divepoints.at(i);
		if (dp.time > minutes ){
			row = i;
			break;
		}
	}

	// add the new stop
	beginInsertRows(QModelIndex(), row, row);
	divedatapoint point;
	point.depth = meters;
	point.time = minutes;
	if (row == 0){
		point.o2 = 209;
		point.he = 0;
		point.po2 = 0;
	}else{
		divedatapoint before = at(row-1);
		point.o2 = before.o2;
		point.he = before.he;
		point.po2 = 0;
	}
	divepoints.append( point );
	std::sort(divepoints.begin(), divepoints.end(), divePointsLessThan);
	endInsertRows();
	return row;
}

void DivePlannerPointsModel::editStop(int row, divedatapoint newData)
{
	divepoints[row] = newData;
	std::sort(divepoints.begin(), divepoints.end(), divePointsLessThan);
	emit dataChanged(createIndex(0, 0), createIndex(rowCount()-1, COLUMNS-1));
}

divedatapoint DivePlannerPointsModel::at(int row)
{
	return divepoints.at(row);
}

void DivePlannerPointsModel::remove(const QModelIndex& index)
{
	if (index.column() != REMOVE)
		return;

	beginRemoveRows(QModelIndex(), index.row(), index.row());
	divepoints.remove(index.row());
	endRemoveRows();
}

struct diveplan DivePlannerPointsModel::getDiveplan()
{
	return diveplan;
}

void DivePlannerPointsModel::cancelPlan()
{
	if(rowCount()){
		if (QMessageBox::warning(mainWindow(), tr("Save the Plan?"),
			tr("You have a working plan, \n are you sure that you wanna cancel it?"),
				QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok){
				return;
			}
	}

	beginRemoveRows(QModelIndex(), 0, rowCount()-1);
	divepoints.clear();
	endRemoveRows();
	emit planCanceled();
}

void DivePlannerPointsModel::createTemporaryPlan()
{
	// This needs to be done in the following steps:
	// Get the user-input and calculate the dive info
	// Not sure if this is the place to create the diveplan...
	// We just start with a surface node at time = 0
	struct divedatapoint *dp = create_dp(0, 0, 209, 0, 0);
	dp->entered = TRUE;
	diveplan.dp = dp;
	int lastIndex = -1;
	for(int i = 0; i < rowCount(); i++){
		divedatapoint p = at(i);
		int deltaT = lastIndex != -1 ? p.time - at(lastIndex).time : p.time;
		lastIndex = i;
		dp = plan_add_segment(&diveplan, deltaT, p.depth, p.o2, p.he, p.po2);
	}
#if DEBUG_PLAN
	dump_plan(&diveplan);
#endif
	char *cache = NULL;
	tempDive = NULL;
	char *errorString = NULL;
	plan(&diveplan, &cache, &tempDive, &errorString);
#if DEBUG_PLAN
	dump_plan(&diveplan);
#endif
}

void DivePlannerPointsModel::deleteTemporaryPlan()
{
	deleteTemporaryPlan(diveplan.dp);
	delete_single_dive(get_divenr(tempDive));
	tempDive = NULL;
}

void DivePlannerPointsModel::deleteTemporaryPlan(struct divedatapoint *dp)
{
	if (!dp){
		return;
	}

	deleteTemporaryPlan(dp->next);
	free(dp);
}
