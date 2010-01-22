#include <QtCore>
#include <QtNetwork>
#include <QtGui>

#include "../../overlay/overlay.h"

#include "Timer.h"

class OverlayWidget : public QWidget {
	Q_OBJECT
	
	protected:
		QImage img;
		OverlayMsg om;
		QLocalSocket *qlsSocket;
		QSharedMemory *qsmMem;
		QRect qrActive;
		
		unsigned int uiWidth, uiHeight;
		
		void resizeEvent(QResizeEvent *);
		void paintEvent(QPaintEvent *);
		void init(const QSize &);
		void detach();
	protected slots:
		void connected();
		void disconnected();
		void readyRead();
		void error(QLocalSocket::LocalSocketError);
	public:
		OverlayWidget(QWidget *p = NULL);
};

OverlayWidget::OverlayWidget(QWidget *p) : QWidget(p) {
	qlsSocket = NULL;
	qsmMem = NULL;
	uiWidth = uiHeight = 0;
}

void OverlayWidget::resizeEvent(QResizeEvent *evt) {
	qWarning() << "resize";
	QWidget::resizeEvent(evt);

	if (! img.isNull())
		img = img.scaled(evt->size().width(), evt->size().height());
	init(evt->size());
}

void OverlayWidget::paintEvent(QPaintEvent *evt) {
	qWarning() << "paint";

	if (! qlsSocket || qlsSocket->state() == QLocalSocket::UnconnectedState) {
		detach();

		qlsSocket = new QLocalSocket(this);
		connect(qlsSocket, SIGNAL(connected()), this, SLOT(connected()));
		connect(qlsSocket, SIGNAL(disconnected()), this, SLOT(disconnected()));
		connect(qlsSocket, SIGNAL(readyRead()), this, SLOT(readyRead()));
		connect(qlsSocket, SIGNAL(error(QLocalSocket::LocalSocketError)), this, SLOT(error(QLocalSocket::LocalSocketError)));
		qlsSocket->connectToServer(QLatin1String("MumbleOverlayPipe"));
	}

	QPainter painter(this);
	painter.fillRect(0, 0, width(), height(), QColor(128,0,128));
	painter.setClipRect(qrActive);
	painter.drawImage(0, 0, img);
}

void OverlayWidget::init(const QSize &sz) {

	OverlayMsg m;	
	m.omh.uiMagic = OVERLAY_MAGIC_NUMBER;
	m.omh.uiType = OVERLAY_MSGTYPE_INIT;
	m.omh.iLength = sizeof(OverlayMsgInit);
	m.omi.uiWidth = sz.width();
	m.omi.uiHeight = sz.height();
	
	if (qlsSocket && qlsSocket->state() == QLocalSocket::ConnectedState)
		qlsSocket->write(m.headerbuffer, sizeof(OverlayMsgHeader) + sizeof(OverlayMsgInit));
	
	img = QImage(sz.width(), sz.height(), QImage::Format_ARGB32);
}

void OverlayWidget::detach() {
	if (qlsSocket) {
		qlsSocket->abort();
		qlsSocket->deleteLater();
		qlsSocket = NULL;
	}
	if (qsmMem) {
		qsmMem = NULL;
	}
}

void OverlayWidget::connected() {
	qWarning() << "connected";
	
	om.omh.iLength = -1;
	
	init(size());
}

void OverlayWidget::disconnected() {
	qWarning() << "disconnected";

	QLocalSocket *qls = qobject_cast<QLocalSocket *>(sender());
	if (qls == qlsSocket) {
		detach();
	}
}

void OverlayWidget::error(QLocalSocket::LocalSocketError) {
	qWarning() << "error";
	disconnected();
}

void OverlayWidget::readyRead() {
	QLocalSocket *qls = qobject_cast<QLocalSocket *>(sender());
	if (qls != qlsSocket)
		return;

	while(1) {
		int ready = qlsSocket->bytesAvailable();
		
		if (om.omh.iLength == -1) {
			if (ready < sizeof(OverlayMsgHeader))
				break;
			else {
				qlsSocket->read(reinterpret_cast<char *>(om.headerbuffer), sizeof(OverlayMsgHeader));
				if ((om.omh.uiMagic != OVERLAY_MAGIC_NUMBER) || (om.omh.iLength < 0) || (om.omh.iLength > sizeof(OverlayMsgShmem))) {
					detach();
					return;
				}
				ready -= sizeof(OverlayMsgHeader);
			}
		}

		if (ready >= om.omh.iLength) {
			int length = qlsSocket->read(om.msgbuffer, om.omh.iLength);

			qWarning() << length << om.omh.uiType;

			if (length != om.omh.iLength) {
					detach();
					return;
			}

			switch(om.omh.uiType) {
				case OVERLAY_MSGTYPE_SHMEM:
					{
						OverlayMsgShmem *oms = & om.oms;
						QString key = QString::fromUtf8(oms->a_cName, length);
						qWarning() << "SHMAT" << key;
						if (qsmMem)
							delete qsmMem;
						qsmMem = new QSharedMemory(key, qlsSocket);
						if (! qsmMem->attach(QSharedMemory::ReadOnly)) { // || (qsmMem->size() != width() * height() * 4)) {
							qWarning() << "SHMEM FAIL" << qsmMem->errorString();
							delete qsmMem;
							qsmMem = NULL;
						} else if (qsmMem->size() < width() * height() * 4) {
							qWarning() << "SHMEM SIZE" << qsmMem->size();
							delete qsmMem;
							qsmMem = NULL;
						} else {
							qWarning() << "SHMEM" << qsmMem->size();
						}
					}
					break;
				case OVERLAY_MSGTYPE_BLIT:
					{
						OverlayMsgBlit *omb = & om.omb;
						length -= sizeof(OverlayMsgBlit);

						qWarning() << "BLIT" << omb->x << omb->y << omb->w << omb->h;

						if (! qsmMem || ! qsmMem->isAttached())
							break;
														
						if (((omb->x + omb->w) > img.width()) || 
							((omb->y + omb->h) > img.height()))
								break;
								

						for(int y = 0; y < omb->h; ++y) {
							unsigned char *src = reinterpret_cast<unsigned char *>(qsmMem->data()) + 4 * (width() * (y + omb->y) + omb->x);
							unsigned char *dst = img.scanLine(y + omb->y) + omb->x * 4;
							memcpy(dst, src, omb->w * 4);
						}

						update();
					}
					break;
				case OVERLAY_MSGTYPE_ACTIVE:
					{
						OverlayMsgActive *oma = & om.oma;
						
						qWarning() << "ACTIVE" << oma->x << oma->y << oma->w << oma->h;
						
						qrActive = QRect(oma->x, oma->y, oma->w, oma->h);
						update();
					};
					break;
				default:
					break;
			}
			om.omh.iLength = -1;
			ready -= length;
		} else {
			break;
		}
	}
}

class TestWin : public QObject {
	Q_OBJECT
	
	protected:
		QWidget *qw;
	public:
		TestWin();
};

TestWin::TestWin() {
	QMainWindow *qmw=new QMainWindow();
	qmw->setObjectName(QLatin1String("main"));
	
	OverlayWidget *ow = new OverlayWidget();
	qmw->setCentralWidget(ow);
	
	qmw->show();
	qmw->resize(1280,720);
	
	QMetaObject::connectSlotsByName(this);
}

int main(int argc, char **argv) {
	QApplication a(argc, argv);
	
	TestWin t;

	return a.exec();
}

#include "Overlay.moc"
