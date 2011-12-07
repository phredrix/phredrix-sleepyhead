/*
 SleepLib Machine Class Implementation
 Copyright (c)2011 Mark Watkins <jedimark@users.sourceforge.net>
 License: GPL
*/

#include <QApplication>
#include <QDir>
#include <QProgressBar>
#include <QDebug>
#include <QString>
#include <QObject>
#include <tr1/random>
#include <sys/time.h>

#include "machine.h"
#include "profiles.h"
#include <algorithm>
#include "SleepLib/schema.h"

extern QProgressBar * qprogress;


qint64 timezoneOffset() {
    static bool ok=false;
    static qint64 _TZ_offset=0;

    if (ok) return _TZ_offset;
    QDateTime d1=QDateTime::currentDateTime();
    QDateTime d2=d1;
    d1.setTimeSpec(Qt::UTC);
    _TZ_offset=d2.secsTo(d1);
    _TZ_offset*=1000L;
    return _TZ_offset;
}

//////////////////////////////////////////////////////////////////////////////////////////
// Machine Base-Class implmementation
//////////////////////////////////////////////////////////////////////////////////////////
Machine::Machine(Profile *p,MachineID id)
{
    day.clear();
    highest_sessionid=0;
    profile=p;
    if (!id) {
        std::tr1::minstd_rand gen;
        std::tr1::uniform_int<MachineID> unif(1, 0x7fffffff);
        gen.seed((unsigned int) time(NULL));
        MachineID temp;
        do {
            temp = unif(gen); //unif(gen) << 32 |
        } while (profile->machlist.find(temp)!=profile->machlist.end());

        m_id=temp;

    } else m_id=id;
    //qDebug() << "Create Machine: " << hex << m_id; //%lx",m_id);
    m_type=MT_UNKNOWN;
    firstsession=true;
}
Machine::~Machine()
{
    qDebug() << "Destroy Machine";
    for (QMap<QDate,Day *>::iterator d=day.begin();d!=day.end();d++) {
        delete d.value();
    }
}
Session *Machine::SessionExists(SessionID session)
{
    if (sessionlist.find(session)!=sessionlist.end()) {
        return sessionlist[session];
    } else {
        return NULL;
    }
}

Day *Machine::AddSession(Session *s,Profile *p)
{
    if (!s) {
        qWarning() << "Empty Session in Machine::AddSession()";
        return NULL;
    }
    if (!p) {
        qWarning() << "Empty Profile in Machine::AddSession()";
        return NULL;
    }
    if (s->session()>highest_sessionid)
        highest_sessionid=s->session();


    QTime split_time(12,0,0);
    if (PROFILE.Exists("DaySplitTime")) {
        split_time=PROFILE["DaySplitTime"].toTime();
    }
    int combine_sessions;
    if (PROFILE.Exists("CombineCloserSessions")) {
        combine_sessions=PROFILE["CombineCloserSessions"].toInt(); // In Minutes
    } else combine_sessions=0;

    int ignore_sessions;
    if (PROFILE.Exists("IgnoreShorterSessions")) {
        ignore_sessions=PROFILE["IgnoreShorterSessions"].toInt(); // In Minutes
    } else ignore_sessions=0;

    int session_length=s->last()-s->first();
    session_length/=60000;

    sessionlist[s->session()]=s; // To make sure it get's saved later even if it's not wanted.

    QDateTime d2=QDateTime::fromTime_t(s->first()/1000);

    QDate date=d2.date();
    QTime time=d2.time();

    QMap<QDate,Day *>::iterator dit,nextday;


    bool combine_next_day=false;
    int closest_session=0;

    if (time<split_time) {
        date=date.addDays(-1);
    } else if (combine_sessions > 0) {
        dit=day.find(date.addDays(-1)); // Check Day Before
        if (dit!=day.end()) {
            QDateTime lt=QDateTime::fromTime_t(dit.value()->last()/1000);
            closest_session=lt.secsTo(d2)/60;
            if (closest_session<combine_sessions) {
                date=date.addDays(-1);
            }
        } else {
            nextday=day.find(date.addDays(1));// Check Day Afterwards
            if (nextday!=day.end()) {
                QDateTime lt=QDateTime::fromTime_t(nextday.value()->first()/1000);
                closest_session=d2.secsTo(lt)/60;
                if (closest_session < combine_sessions) {
                    // add todays here. pull all tomorrows records to this date.
                    combine_next_day=true;
                }
            }
        }
    }

    if (session_length<ignore_sessions) {
        //if (!closest_session || (closest_session>=60))
        return NULL;
    }

    if (!firstsession) {
        if (firstday>date) firstday=date;
        if (lastday<date) lastday=date;
    } else {
        firstday=lastday=date;
        firstsession=false;
    }


    Day *dd=NULL;
    dit=day.find(date);
    if (dit==day.end()) {
        //QString dstr=date.toString("yyyyMMdd");
        //qDebug("Adding Profile Day %s",dstr.toAscii().data());
        dd=new Day(this);
        day[date]=dd;
        // Add this Day record to profile
        p->AddDay(date,dd,m_type);
    } else {
        dd=*dit;
    }
    dd->AddSession(s);

    if (combine_next_day) {
        for (QVector<Session *>::iterator i=nextday.value()->begin();i!=nextday.value()->end();i++) {
            dd->AddSession(*i);
        }
        QMap<QDate,QVector<Day *> >::iterator nd=p->daylist.find(date.addDays(1));
        for (QVector<Day *>::iterator i=nd->begin();i!=nd->end();i++) {
            if (*i==nextday.value()) {
                nd.value().erase(i);
            }
        }
        day.erase(nextday);
    }
    return dd;
}

// This functions purpose is murder and mayhem... It deletes all of a machines data.
// Therefore this is the most dangerous function in this software..
bool Machine::Purge(int secret)
{
    // Boring api key to stop this function getting called by accident :)
    if (secret!=3478216) return false;


    // It would be joyous if this function screwed up..
    QString path=profile->Get("DataFolder")+"/"+hexid();

    QDir dir(path);

    if (!dir.exists()) // It doesn't exist anyway.
        return true;
    if (!dir.isReadable())
        return false;


    qDebug() << "Purging " << QDir::toNativeSeparators(path);

    dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    dir.setSorting(QDir::Name);

    QFileInfoList list=dir.entryInfoList();
    int could_not_kill=0;

    for (int i=0;i<list.size();++i) {
        QFileInfo fi=list.at(i);
        QString fullpath=fi.canonicalFilePath();
        //int j=fullpath.lastIndexOf(".");

        QString ext_s=fullpath.section('.',-1);//right(j);
        bool ok;
        ext_s.toInt(&ok,10);
        if (ok) {
            qDebug() << "Deleting " << fullpath;
            dir.remove(fullpath);
        } else could_not_kill++;

    }
    dir.remove(path+"/channels.dat");
    if (could_not_kill>0) {
      //  qWarning() << "Could not purge path\n" << path << "\n\n" << could_not_kill << " file(s) remain.. Suggest manually deleting this path\n";
    //    return false;
    }

    return true;
}

const quint32 channel_version=1;


bool Machine::Load()
{
    QString path=profile->Get("DataFolder")+"/"+hexid();

    QDir dir(path);
    qDebug() << "Loading " << path;

    if (!dir.exists() || !dir.isReadable())
        return false;

/*    QString fn=path+"/channels.dat";
    QFile cf(fn);
    cf.open(QIODevice::ReadOnly);
    QDataStream in(&cf);
    in.setVersion(QDataStream::Qt_4_6);
    in.setByteOrder(QDataStream::LittleEndian);

    quint32 tmp;
    in >> tmp;
    if (magic!=tmp) {
        qDebug() << "Machine Channel file format is wrong" << fn;
    }
    in >> tmp;
    if (tmp!=channel_version) {
        qDebug() << "Machine Channel file format is wrong" << fn;
    }
    qint32 tmp2;
    in >> tmp2;
    if (tmp2!=m_id) {
        qDebug() << "Machine Channel file format is wrong" << fn;
    }
    in >> m_channels;
    cf.close(); */

    dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    dir.setSorting(QDir::Name);

    QFileInfoList list=dir.entryInfoList();

    typedef QVector<QString> StringList;
    QMap<SessionID,StringList> sessfiles;
    QMap<SessionID,StringList>::iterator s;

    QString fullpath,ext_s,sesstr;
    int ext;
    SessionID sessid;
    bool ok;
    for (int i=0;i<list.size();i++) {
        QFileInfo fi=list.at(i);
        fullpath=fi.canonicalFilePath();
        ext_s=fi.fileName().section(".",-1);
        ext=ext_s.toInt(&ok,10);
        if (!ok) continue;
        sesstr=fi.fileName().section(".",0,-2);
        sessid=sesstr.toLong(&ok,16);
        if (!ok) continue;
        if (sessfiles[sessid].capacity()==0) sessfiles[sessid].resize(3);
        sessfiles[sessid][ext]=fi.canonicalFilePath();
    }

    int size=sessfiles.size();
    int cnt=0;
    for (s=sessfiles.begin(); s!=sessfiles.end(); s++) {
        cnt++;
        if ((cnt % 10)==0)
            if (qprogress) qprogress->setValue((float(cnt)/float(size)*100.0));

        Session *sess=new Session(this,s.key());

        if (sess->LoadSummary(s.value()[0])) {
             sess->SetEventFile(s.value()[1]);
             AddSession(sess,profile);
        } else {
            qWarning() << "Error unpacking summary data";
            delete sess;
        }
    }
    if (qprogress) qprogress->setValue(100);
    return true;
}
bool Machine::SaveSession(Session *sess)
{
    QString path=profile->Get("DataFolder")+"/"+hexid();
    if (sess->IsChanged()) sess->Store(path);
    return true;
}

bool Machine::Save()
{
    //int size;
    int cnt=0;

    QString path=profile->Get("DataFolder")+"/"+hexid();
    QDir dir(path);
    if (!dir.exists()) {
        dir.mkdir(path);
    }

    QHash<SessionID,Session *>::iterator s;

    m_savelist.clear();
    for (s=sessionlist.begin(); s!=sessionlist.end(); s++) {
        cnt++;
        if ((*s)->IsChanged()) {
            m_savelist.push_back(*s);
        }
    }
    savelistCnt=0;
    savelistSize=m_savelist.size();
    if (!PROFILE["EnableMultithreading"].toBool()) {
        for (int i=0;i<savelistSize;i++) {
            qprogress->setValue(66.0+(float(savelistCnt)/float(savelistSize)*33.0));
            QApplication::processEvents();
            Session *s=m_savelist.at(i);
            s->UpdateSummaries();
            s->Store(path);
            s->TrashEvents();
            savelistCnt++;

        }
        return true;
    }
    int threads=QThread::idealThreadCount();
    savelistSem=new QSemaphore(threads);
    savelistSem->acquire(threads);
    QVector<SaveThread*>thread;
    for (int i=0;i<threads;i++) {
        thread.push_back(new SaveThread(this,path));
        QObject::connect(thread[i],SIGNAL(UpdateProgress(int)),qprogress,SLOT(setValue(int)));
        thread[i]->start();
    }
    while (!savelistSem->tryAcquire(threads,250)) {
        //qDebug() << savelistSem->available();
        if (qprogress) {
        //    qprogress->setValue(66.0+(float(savelistCnt)/float(savelistSize)*33.0));
           QApplication::processEvents();
        }
    }

    for (int i=0;i<threads;i++) {
        while (thread[i]->isRunning()) {
            SaveThread::msleep(250);
            QApplication::processEvents();
        }
        delete thread[i];
    }

    delete savelistSem;
    return true;
}

/*SaveThread::SaveThread(Machine *m,QString p)
{
    machine=m;
    path=p;
} */

void SaveThread::run()
{
    while (Session *sess=machine->popSaveList()) {
        int i=66.0+(float(machine->savelistCnt)/float(machine->savelistSize)*33.0);
        emit UpdateProgress(i);
        sess->UpdateSummaries();
        sess->Store(path);
        sess->TrashEvents();
    }
    machine->savelistSem->release(1);
}

Session *Machine::popSaveList()
{

    Session *sess=NULL;
    savelistMutex.lock();
    if (m_savelist.size()>0) {
        sess=m_savelist.at(0);
        m_savelist.pop_front();
        savelistCnt++;
    }
    savelistMutex.unlock();
    return sess;
}

//////////////////////////////////////////////////////////////////////////////////////////
// CPAP implmementation
//////////////////////////////////////////////////////////////////////////////////////////
CPAP::CPAP(Profile *p,MachineID id):Machine(p,id)
{
    m_type=MT_CPAP;
}

CPAP::~CPAP()
{
}

//////////////////////////////////////////////////////////////////////////////////////////
// Oximeter Class implmementation
//////////////////////////////////////////////////////////////////////////////////////////
Oximeter::Oximeter(Profile *p,MachineID id):Machine(p,id)
{
    m_type=MT_OXIMETER;
}

Oximeter::~Oximeter()
{
}

//////////////////////////////////////////////////////////////////////////////////////////
// SleepStage Class implmementation
//////////////////////////////////////////////////////////////////////////////////////////
SleepStage::SleepStage(Profile *p,MachineID id):Machine(p,id)
{
    m_type=MT_SLEEPSTAGE;
}
SleepStage::~SleepStage()
{
}





