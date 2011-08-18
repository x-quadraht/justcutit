// JustCutIt editor
// Author: Max Schwarz <Max@x-quadraht.de>

#include "editor.h"

#include <QtGui/QVBoxLayout>

#include <QtCore/QTimer>
#include <QtCore/QFile>
#include <QtGui/QImage>
#include <QtGui/QFileDialog>
#include <QtGui/QMessageBox>

#include "ui_editor.h"
#include "gldisplay.h"

#include <stdio.h>

#include "io_http.h"

Editor::Editor(QWidget* parent)
 : QWidget(parent)
 , m_frameIdx(0)
 , m_headFrame(0)
 , m_cutPointModel(&m_cutPoints)
{
	m_ui = new Ui_Editor;
	m_ui->setupUi(this);
	
	for(int i = 0; i < NUM_FRAMES; ++i)
		m_frameBuffer[i] = avcodec_alloc_frame();
	
	QTimer::singleShot(0, this, SLOT(loadFile()));
	
	connect(m_ui->nextButton, SIGNAL(clicked()), SLOT(seek_nextFrame()));
	connect(m_ui->nextSecondButton, SIGNAL(clicked()), SLOT(seek_plus1Second()));
	connect(m_ui->next30SecButton, SIGNAL(clicked()), SLOT(seek_plus30Sec()));
	connect(m_ui->prevButton, SIGNAL(clicked()), SLOT(seek_prevFrame()));
	
	connect(m_ui->timeSlider, SIGNAL(sliderMoved(int)), SLOT(seek_slider(int)));
	
	connect(m_ui->cutOutButton, SIGNAL(clicked()), SLOT(cut_cutOutHere()));
	connect(m_ui->cutInButton, SIGNAL(clicked()), SLOT(cut_cutInHere()));
	
	m_ui->cutPointView->setModel(&m_cutPointModel);
	m_ui->cutPointView->setRootIndex(QModelIndex());
	
	connect(m_ui->cutPointView, SIGNAL(activated(QModelIndex)), SLOT(cut_pointActivated(QModelIndex)));
	
	QStyle* style = QApplication::style();
	
	m_ui->cutlistOpenButton->setIcon(QIcon::fromTheme("document-open"));
	m_ui->cutlistSaveButton->setIcon(QIcon::fromTheme("document-save"));
	m_ui->cutlistDelItemButton->setIcon(QIcon::fromTheme("list-remove"));
	
	connect(m_ui->cutlistOpenButton, SIGNAL(clicked()), SLOT(cut_openList()));
	connect(m_ui->cutlistSaveButton, SIGNAL(clicked()), SLOT(cut_saveList()));
}

Editor::~Editor()
{
	for(int i = 0; i < NUM_FRAMES; ++i)
		av_free(m_frameBuffer[i]);
}

void Editor::loadFile()
{
	const char *filename = "/home/max/Downloads/Ben Hur.ts";
// 	const char *filename = "http://192.168.178.48:49152/content/internal-recordings/0/record/3144/recording.ts";
	
// 	m_stream = avformat_alloc_context();
// 	m_stream->pb = io_http_create(filename);
	m_stream = 0;
	
	if(avformat_open_input(&m_stream, filename, NULL, NULL) != 0)
	{
		fprintf(stderr, "Fatal: Could not open input stream\n");
		
		return;
	}
	
	if(avformat_find_stream_info(m_stream, NULL) < 0)
	{
		fprintf(stderr, "Fatal: Could not find stream information\n");
		return;
	}
	
	av_dump_format(m_stream, 0, "/home/max/Downloads/Ben Hur.ts", false);
	
	m_videoCodecCtx = 0;
	for(int i = 0; i < m_stream->nb_streams; ++i)
	{
		AVStream* stream = m_stream->streams[i];
		
		if(stream->codec->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			m_videoID = i;
			m_videoCodecCtx = stream->codec;
			break;
		}
	}
	
	if(!m_videoCodecCtx)
	{
		fprintf(stderr, "Fatal: Could not find video stream\n");
		return;
	}
	
	m_videoCodec = avcodec_find_decoder(m_videoCodecCtx->codec_id);
	if(!m_videoCodec)
	{
		fprintf(stderr, "Fatal: Unsupported codec\n");
		return;
	}
	
	if(avcodec_open2(m_videoCodecCtx, m_videoCodec, NULL) < 0)
	{
		fprintf(stderr, "Fatal: Could not open codec\n");
		return;
	}
	
	readFrame(true);
	
	initBuffer();
	resetBuffer();
	
	m_timeStampStart = m_frameTimestamps[0];
	m_videoTimeBase = av_q2d(m_stream->streams[m_videoID]->time_base);
	
	printf("File duration is % 5.2fs\n", (float)m_stream->duration / AV_TIME_BASE);
	m_ui->timeSlider->setMaximum(m_stream->duration / AV_TIME_BASE);
	
	int w = m_videoCodecCtx->width;
	int h = m_videoCodecCtx->height;
	
	m_ui->videoWidget->setSize(w, h);
	m_ui->cutVideoWidget->setSize(w, h);
	
	displayCurrentFrame();
}

void Editor::readFrame(bool needKeyFrame)
{
	AVPacket packet;
	AVFrame frame;
	int frameFinished;
	
	while(av_read_frame(m_stream, &packet) == 0)
	{
		if(packet.stream_index != m_videoID)
			continue;
		
		if(avcodec_decode_video2(m_videoCodecCtx, &frame, &frameFinished, &packet) < 0)
		{
			fprintf(stderr, "Fatal: Could not decode packet\n");
			return;
		}
		
		if(!frameFinished)
			continue;
		
		if(m_videoCodecCtx->pix_fmt != PIX_FMT_YUV420P)
		{
			printf("Fatal: Format %d is unsupported.\n", m_videoCodecCtx->pix_fmt);
			return;
		}
		
		m_frameTimestamps[m_headFrame] = packet.dts;
		av_picture_copy(
			(AVPicture*)m_frameBuffer[m_headFrame],
			(AVPicture*)&frame,
			PIX_FMT_YUV420P,
			m_videoCodecCtx->width,
			m_videoCodecCtx->height
		);
		
		av_free_packet(&packet);
		
		if(!needKeyFrame)
			return;
		
		if(frame.key_frame)
			return;
	}
}

void Editor::displayCurrentFrame()
{
	m_ui->videoWidget->paintFrame(
		m_frameBuffer[m_frameIdx]
	);
	
	m_ui->frameTypeLabel->setText(QString::number(m_frameBuffer[m_frameIdx]->key_frame));
	m_ui->timeStampLabel->setText(QString("%1s").arg(frameTime(m_frameIdx), 7, 'f', 4));
	m_ui->headIdxLabel->setText(QString::number(m_headFrame));
	m_ui->frameIdxLabel->setText(QString::number(m_frameIdx));
	
	if(!m_ui->timeSlider->isSliderDown())
	{
		m_ui->timeSlider->blockSignals(true);
		m_ui->timeSlider->setValue((m_frameTimestamps[m_frameIdx] - m_timeStampStart) * m_videoTimeBase);
		m_ui->timeSlider->blockSignals(false);
	}
}

void Editor::pause()
{
}

void Editor::seek_nextFrame(bool display)
{
	if(++m_frameIdx == NUM_FRAMES)
		m_frameIdx = 0;
	
	if(m_frameIdx == m_headFrame)
	{
		readFrame();
		if(++m_headFrame == NUM_FRAMES)
		{
			m_headFrame = 0;
			m_fullBuffer = true;
		}
	}
	
	if(display)
		displayCurrentFrame();
}

float Editor::frameTime(int idx)
{
	if(idx == -1)
		idx = m_frameIdx;
	
	return m_videoTimeBase *
		(m_frameTimestamps[idx] - m_timeStampStart);
}

void Editor::seek_time(float seconds, bool display)
{
	int ts = m_timeStampStart + seconds / m_videoTimeBase;
	int min_ts = ts - 2.0 / m_videoTimeBase;
	int max_ts = ts;
	
	avcodec_flush_buffers(m_videoCodecCtx);
	
	if(avformat_seek_file(m_stream, m_videoID, min_ts, ts, max_ts, 0) < 0)
	{
		fprintf(stderr, "Fatal: could not seek\n");
		return;
	}
	
	resetBuffer();
	
	if(display)
		displayCurrentFrame();
}

void Editor::seek_timeExact(float seconds, bool display)
{
	for(int i = 0; i == 0 || frameTime() >= seconds; ++i)
		seek_time(seconds - 1.0 * i, false);
	
	if(seconds - frameTime() > 5.0)
	{
		printf("WARNING: Big gap: dest is %f, frameTime is %f\n", seconds, frameTime());
	}
	
	while(frameTime() < seconds - 0.002)
		seek_nextFrame(false);
	
	if(display)
		displayCurrentFrame();
}

void Editor::seek_timeExactBefore(float seconds, bool)
{
	seek_timeExact(seconds, false);
	
	seek_prevFrame();
}

void Editor::resetBuffer()
{
	m_headFrame = 0;
	m_frameIdx = 0;
	m_fullBuffer = false;
	
	readFrame(true);
	m_headFrame++;
}

void Editor::seek_prevFrame()
{
	float time = frameTime();
	
	if(time == 0)
		return;
	
	if(--m_frameIdx < 0 && m_fullBuffer)
		m_frameIdx = NUM_FRAMES - 1;
	
	if(m_frameIdx == m_headFrame || m_frameIdx < 0)
	{
		seek_timeExactBefore(time);
		return;
	}
	
	displayCurrentFrame();
}

void Editor::seek_plus1Second()
{
	seek_time(frameTime() + 1.0);
}

void Editor::seek_plus30Sec()
{
	seek_time(frameTime() + 30.0);
}

void Editor::seek_minus1Second()
{
	seek_time(frameTime() - 1.0);
}

void Editor::seek_minus30Sec()
{
	seek_time(frameTime() - 30.0);
}

void Editor::initBuffer()
{
	int w = m_videoCodecCtx->width;
	int h = m_videoCodecCtx->height;
	
	for(int i = 0; i < NUM_FRAMES; ++i)
	{
		avpicture_fill(
			(AVPicture*)m_frameBuffer[i],
			(uint8_t*)av_malloc(avpicture_get_size(
				PIX_FMT_YUV420P,
				w, h
			)),
			PIX_FMT_YUV420P,
			w, h
		);
	}
}

void Editor::seek_slider(int value)
{
	float time = value;
	
	printf("seeking to % 3.3f\n", time);
	
	seek_time(time);
	
	printf(" => % 3.3f\n", frameTime());
}

void Editor::cut_cut(CutPoint::Direction dir)
{
	int w = m_videoCodecCtx->width;
	int h = m_videoCodecCtx->height;
	
	AVFrame* frame = avcodec_alloc_frame();
	
	avpicture_fill(
		(AVPicture*)frame,
		(uint8_t*)av_malloc(avpicture_get_size(
			PIX_FMT_YUV420P,
			w, h
		)),
		PIX_FMT_YUV420P,
		w, h
	);
	
	av_picture_copy(
		(AVPicture*)frame,
		(AVPicture*)m_frameBuffer[m_frameIdx],
		PIX_FMT_YUV420P,
		w, h
	);
	
	int num = m_cutPoints.addCutPoint(frameTime(), dir, frame);
	QModelIndex idx = m_cutPointModel.idxForNum(num);
	m_ui->cutPointView->setCurrentIndex(idx);
	cut_pointActivated(idx);
}

void Editor::cut_cutOutHere()
{
	cut_cut(CutPoint::CUT_OUT);
}

void Editor::cut_cutInHere()
{
	cut_cut(CutPoint::CUT_IN);
}

void Editor::cut_pointActivated(QModelIndex idx)
{
	CutPoint* point = m_cutPointModel.cutPointForIdx(idx);
	
	m_ui->cutVideoWidget->paintFrame(point->img);
	if(fabs(frameTime() - point->time) > 0.005)
	{
		seek_timeExactBefore(point->time);
		seek_nextFrame();
	}
}

void Editor::cut_openList()
{
	QString filename = QFileDialog::getOpenFileName(
		this,
		"Open cutlist",
		QString(),
		"Cutlists (*.cut)"
	);
	
	if(filename.isNull())
		return;
	
	QFile file(filename);
	if(!file.open(QIODevice::ReadOnly))
	{
		QMessageBox::critical(this, "Error", "Could not open cutlist file");
		return;
	}
	
	if(!m_cutPoints.readFrom(&file))
	{
		QMessageBox::critical(this, "Error", "Cutlist file is damaged");
		return;
	}
	
	file.close();
	
	// Generate images
	int w = m_videoCodecCtx->width;
	int h = m_videoCodecCtx->height;
	
	for(int i = 0; i < m_cutPoints.count(); ++i)
	{
		CutPoint& p = m_cutPoints.at(i);
		
		seek_timeExact(p.time, false);
		
		p.img = avcodec_alloc_frame();
		avpicture_fill(
			(AVPicture*)p.img,
			(uint8_t*)av_malloc(avpicture_get_size(
				PIX_FMT_YUV420P,
				w, h
			)),
			PIX_FMT_YUV420P,
			w, h
		);
		
		av_picture_copy(
			(AVPicture*)p.img,
			(AVPicture*)m_frameBuffer[m_frameIdx],
			PIX_FMT_YUV420P,
			w, h
		);
	}
	
	if(m_cutPoints.count())
	{
		QModelIndex first = m_cutPointModel.idxForNum(0);
		m_ui->cutPointView->setCurrentIndex(first);
		cut_pointActivated(first);
	}
}

void Editor::cut_saveList()
{
	QString filename = QFileDialog::getSaveFileName(
		this,
		"Open cutlist",
		QString(),
		"Cutlists (*.cut)"
	);
	
	if(filename.isNull())
		return;
	
	QFile file(filename);
	if(!file.open(QIODevice::WriteOnly))
	{
		QMessageBox::critical(this, "Error", "Could not open output file");
		return;
	}
	
	m_cutPoints.writeTo(&file);
	
	file.close();
}

#include "editor.moc"
