/*
 * OPN2 Bank Editor by Wohlstand, a free tool for music bank editing
 * Copyright (c) 2017-2018 Vitaly Novichkov <admin@wohlnet.ru>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QStatusBar>
#include <QSettings>
#include <QUrl>
#include <QMimeData>
#include <QStandardItemModel>

#include "importer.h"
#include "formats_sup.h"
#include "bank_editor.h"
#include "ui_bank_editor.h"
#include "ins_names.h"

#include "FileFormats/ffmt_factory.h"

#include "opl/measurer.h"

#include "common.h"
#include "version.h"

#define INS_INDEX   (Qt::UserRole)
#define INS_BANK_ID (Qt::UserRole + 1)
#define INS_INS_ID  (Qt::UserRole + 2)

static void setInstrumentMetaInfo(QListWidgetItem *item, int index)
{
    item->setData(INS_INDEX, index);
    item->setData(INS_BANK_ID, index / 128);
    item->setData(INS_INS_ID, index % 128);
    item->setToolTip(QString("Bank %1, ID: %2").arg(index / 128).arg(index % 128));
}

static QIcon makeWindowIcon()
{
    QIcon icon;
    icon.addPixmap(QPixmap(":/icons/opn2_16.png"));
    icon.addPixmap(QPixmap(":/icons/opn2_32.png"));
    icon.addPixmap(QPixmap(":/icons/opn2_48.png"));
    return icon;
}

BankEditor::BankEditor(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::BankEditor)
{
    FmBankFormatFactory::registerAllFormats();
    memset(&m_clipboard, 0, sizeof(FmBank::Instrument));
    m_curInst = NULL;
    m_curInstBackup = NULL;
    m_lock = false;
    m_recentFormat = BankFormats::FORMAT_WOHLSTAND_OPN2;
    m_recentNum     = -1;
    m_recentPerc    = false;
    ui->setupUi(this);
    this->setWindowIcon(makeWindowIcon());
    ui->version->setText(QString("%1, v.%2").arg(PROGRAM_NAME).arg(VERSION));
    m_recentMelodicNote = ui->noteToTest->value();
    setMelodic();
    connect(ui->melodic,    SIGNAL(clicked(bool)),  this,   SLOT(setMelodic()));
    connect(ui->percussion, SIGNAL(clicked(bool)),  this,   SLOT(setDrums()));
    loadInstrument();
    #if QT_VERSION >= 0x050000
    this->setWindowFlags(Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                         Qt::WindowCloseButtonHint |
                         Qt::WindowMinimizeButtonHint);
    #else
    this->setWindowFlags(this->windowFlags() & ~Qt::WindowMaximizeButtonHint);
    #endif
    this->setFixedSize(this->window()->width(), this->window()->height());
    m_importer = new Importer(this);
    m_measurer = new Measurer(this);
    connect(ui->actionImport, SIGNAL(triggered()), m_importer, SLOT(show()));
    connect(ui->actionEmulatorNuked, SIGNAL(triggered()), this, SLOT(toggleEmulator()));
    connect(ui->actionEmulatorMame, SIGNAL(triggered()), this, SLOT(toggleEmulator()));
    connect(ui->actionEmulatorGens, SIGNAL(triggered()), this, SLOT(toggleEmulator()));

    /* Hide first 7 SSG-EG items */
    {
        QComboBox *ssgegs[4] = {ui->op1_ssgeg, ui->op2_ssgeg, ui->op3_ssgeg, ui->op4_ssgeg};
        for(size_t i = 0; i < 4; i++)
        {
            QStandardItemModel* model = qobject_cast<QStandardItemModel*>(ssgegs[i]->model());
            for(int j = 1; j < 8; j++)
            {
                QStandardItem* item = model->item(j);
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            }
        }
    }

    loadSettings();
    initAudio();
}

BankEditor::~BankEditor()
{
    #ifdef ENABLE_AUDIO_TESTING
    if (m_audioOut)
        m_audioOut->stop();
    m_generator->stop();
    delete m_audioOut;
    m_audioOut = nullptr;
    #endif
    delete m_measurer;
    delete m_generator;
    delete m_importer;
    delete ui;
}

void BankEditor::loadSettings()
{
    QApplication::setOrganizationName(COMPANY);
    QApplication::setOrganizationDomain(PGE_URL);
    QApplication::setApplicationName("OPN2 FM Banks Editor");
    QSettings setup;
    m_recentPath = setup.value("recent-path").toString();
    m_currentChip = (Generator::OPN_Chips)setup.value("chip-emulator", 0).toInt();

    ui->actionEmulatorNuked->setChecked(false);
    ui->actionEmulatorMame->setChecked(false);
    ui->actionEmulatorGens->setChecked(false);

    switch(m_currentChip)
    {
    case Generator::CHIP_Nuked:
        ui->actionEmulatorNuked->setChecked(true);
        break;
    case Generator::CHIP_GENS:
        ui->actionEmulatorGens->setChecked(true);
        break;
    case Generator::CHIP_MAME:
        ui->actionEmulatorMame->setChecked(true);
        break;
    }
}

void BankEditor::saveSettings()
{
    QSettings setup;
    setup.setValue("recent-path", m_recentPath);
    setup.setValue("chip-emulator", (int)m_currentChip);
}



void BankEditor::closeEvent(QCloseEvent *event)
{
    if(!askForSaving())
    {
        event->ignore();
        return;
    }

    saveSettings();
}

void BankEditor::dragEnterEvent(QDragEnterEvent *e)
{
    if(e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void BankEditor::dropEvent(QDropEvent *e)
{
    this->raise();
    this->setFocus(Qt::ActiveWindowFocusReason);

    foreach(const QUrl &url, e->mimeData()->urls())
    {
        const QString &fileName = url.toLocalFile();
        if(openFile(fileName))
            break; //Only first valid file!
    }
}


void BankEditor::initFileData(QString &filePath)
{
    m_recentPath = QFileInfo(filePath).absoluteDir().absolutePath();
    m_recentBankFilePath = filePath;

    if(!ui->instruments->selectedItems().isEmpty())
    {
        int idOfSelected = ui->instruments->selectedItems().first()->data(Qt::UserRole).toInt();
        if(ui->melodic->isChecked())
            setMelodic();
        else
            setDrums();
        ui->instruments->clearSelection();
        QList<QListWidgetItem *> items = ui->instruments->findItems("*", Qt::MatchWildcard);
        for(int i = 0; i < items.size(); i++)
        {
            if(items[i]->data(Qt::UserRole).toInt() == idOfSelected)
            {
                items[i]->setSelected(true);
                break;
            }
        }
        if(!ui->instruments->selectedItems().isEmpty())
            on_instruments_currentItemChanged(ui->instruments->selectedItems().first(), NULL);
    }
    else
        on_instruments_currentItemChanged(NULL, NULL);

    ui->currentFile->setText(filePath);
    m_bankBackup = m_bank;

    //Set global flags and states
    m_lock = true;
    ui->lfoEnable->setChecked(m_bank.lfo_enabled);
    ui->lfoFrequency->setCurrentIndex(m_bank.lfo_frequency);
    m_lock = false;

    reloadInstrumentNames();
    reloadBanks();
    setCurrentInstrument(m_recentNum, m_recentPerc);
}

void BankEditor::reInitFileDataAfterSave(QString &filePath)
{
    ui->currentFile->setText(filePath);
    m_recentPath = QFileInfo(filePath).absoluteDir().absolutePath();
    m_recentBankFilePath = filePath;
    m_bankBackup = m_bank;
}

bool BankEditor::openFile(QString filePath)
{
    FfmtErrCode err = FmBankFormatFactory::OpenBankFile(filePath, m_bank, &m_recentFormat);
    if(err != FfmtErrCode::ERR_OK)
    {
        QString errText;
        switch(err)
        {
        case FfmtErrCode::ERR_BADFORMAT:
            errText = tr("bad file format");
            break;
        case FfmtErrCode::ERR_NOFILE:
            errText = tr("can't open file");
            break;
        case FfmtErrCode::ERR_NOT_IMLEMENTED:
            errText = tr("reading of this format is not implemented yet");
            break;
        case FfmtErrCode::ERR_UNSUPPORTED_FORMAT:
            errText = tr("unsupported file format");
            break;
        case FfmtErrCode::ERR_UNKNOWN:
            errText = tr("unknown error occouped");
            break;
        case FfmtErrCode::ERR_OK:
            break;
        }
        ErrMessageO(this, errText);
        return false;
    }
    else
    {
        initFileData(filePath);
        statusBar()->showMessage(tr("Bank '%1' has been loaded!").arg(filePath), 5000);
        return true;
    }
}

bool BankEditor::saveBankFile(QString filePath, BankFormats format)
{
    if(format == BankFormats::FORMAT_WOHLSTAND_OPN2)
    {
        if(!m_measurer->doMeasurement(m_bank, m_bankBackup))
            return false;//Measurement was cancelled
    }

    FfmtErrCode err = FmBankFormatFactory::SaveBankFile(filePath, m_bank, format);

    if(err != FfmtErrCode::ERR_OK)
    {
        QString errText;
        switch(err)
        {
        case FfmtErrCode::ERR_BADFORMAT:
            errText = tr("bad file format");
            break;
        case FfmtErrCode::ERR_NOFILE:
            errText = tr("can't open file for write");
            break;
        case FfmtErrCode::ERR_NOT_IMLEMENTED:
            errText = tr("writing into this format is not implemented yet");
            break;
        case FfmtErrCode::ERR_UNSUPPORTED_FORMAT:
            errText = tr("unsupported file format, please define file name extension to choice target file format");
            break;
        case FfmtErrCode::ERR_UNKNOWN:
            errText = tr("unknown error occouped");
            break;
        case FfmtErrCode::ERR_OK:
            break;
        }
        ErrMessageS(this, errText);
        return false;
    }
    else
    {
        //Override 'recently-saved' format
        m_recentFormat = format;
        reInitFileDataAfterSave(filePath);
        statusBar()->showMessage(tr("Bank file '%1' has been saved!").arg(filePath), 5000);
        return true;
    }
}

bool BankEditor::saveInstrumentFile(QString filePath, InstFormats format)
{
    Q_ASSERT(m_curInst);
    FfmtErrCode err = FmBankFormatFactory::SaveInstrumentFile(filePath, *m_curInst, format, ui->percussion->isChecked());
    if(err != FfmtErrCode::ERR_OK)
    {
        QString errText;
        switch(err)
        {
        case FfmtErrCode::ERR_BADFORMAT:
            errText = tr("bad file format");
            break;
        case FfmtErrCode::ERR_NOFILE:
            errText = tr("can't open file for write");
            break;
        case FfmtErrCode::ERR_NOT_IMLEMENTED:
            errText = tr("writing into this format is not implemented yet");
            break;
        case FfmtErrCode::ERR_UNSUPPORTED_FORMAT:
            errText = tr("unsupported file format, please define file name extension to choice target file format");
            break;
        case FfmtErrCode::ERR_UNKNOWN:
            errText = tr("unknown error occouped");
            break;
        case FfmtErrCode::ERR_OK:
            break;
        }
        ErrMessageS(this, errText);
        return false;
    }
    else
    {
        statusBar()->showMessage(tr("Instrument file '%1' has been saved!").arg(filePath), 5000);
        return true;
    }
}

bool BankEditor::saveFileAs()
{
    QString filters         = FmBankFormatFactory::getSaveFiltersList();
    QString selectedFilter  = FmBankFormatFactory::getFilterFromFormat(m_recentFormat, (int)FormatCaps::FORMAT_CAPS_SAVE);
    QString fileToSave      = QFileDialog::getSaveFileName(this, "Save bank file",
                                                           m_recentBankFilePath, filters, &selectedFilter,
                                                           FILE_OPEN_DIALOG_OPTIONS);
    if(fileToSave.isEmpty())
        return false;
    return saveBankFile(fileToSave, FmBankFormatFactory::getFormatFromFilter(selectedFilter));
}

bool BankEditor::saveInstFileAs()
{
    if(!m_curInst)
    {
        QMessageBox::information(this,
                                 tr("Nothing to save"),
                                 tr("No selected instrument to save. Please select an instrument first!"));
        return false;
    }
    QString filters = FmBankFormatFactory::getInstSaveFiltersList();
    QString selectedFilter = FmBankFormatFactory::getInstFilterFromFormat(m_recentInstFormat, (int)FormatCaps::FORMAT_CAPS_SAVE);
    QString fileToSave = QFileDialog::getSaveFileName(this, "Save instrument file",
                                                      m_recentPath, filters, &selectedFilter,
                                                      FILE_OPEN_DIALOG_OPTIONS);
    if(fileToSave.isEmpty())
        return false;
    return saveInstrumentFile(fileToSave, FmBankFormatFactory::getInstFormatFromFilter(selectedFilter));
}

bool BankEditor::askForSaving()
{
    if(m_bank != m_bankBackup)
    {
        QMessageBox::StandardButton res = QMessageBox::question(this, tr("File is not saved"), tr("File is modified and not saved. Do you want to save it?"), QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if((res == QMessageBox::Cancel) || (res == QMessageBox::NoButton))
            return false;
        else if(res == QMessageBox::Yes)
        {
            if(!saveFileAs())
                return false;
        }
    }

    return true;
}

void BankEditor::flushInstrument()
{
    loadInstrument();
    if(m_curInst && ui->percussion->isChecked())
        ui->noteToTest->setValue(m_curInst->percNoteNum);
    sendPatch();
}

void BankEditor::syncInstrumentName()
{
    QListWidgetItem *curInstr = ui->instruments->currentItem();
    if(m_curInst && curInstr)
    {
        curInstr->setText(
            m_curInst->name[0] != '\0' ?
            QString::fromUtf8(m_curInst->name) :
            (m_recentPerc ? getMidiInsNameP(m_recentNum) : getMidiInsNameM(m_recentNum))
        );
    }
}

void BankEditor::on_actionNew_triggered()
{
    if(!askForSaving())
        return;
    m_recentFormat = BankFormats::FORMAT_WOHLSTAND_OPN2;
    ui->currentFile->setText(tr("<Untitled>"));
    ui->instruments->clearSelection();
    m_bank.reset();
    m_bankBackup.reset();
    on_instruments_currentItemChanged(NULL, NULL);
    reloadInstrumentNames();
    reloadBanks();
}

void BankEditor::on_actionOpen_triggered()
{
    if(!askForSaving())
        return;
    QString filters = FmBankFormatFactory::getOpenFiltersList();
    QString fileToOpen;
    fileToOpen = QFileDialog::getOpenFileName(this, "Open bank file",
                                              m_recentPath, filters, nullptr,
                                              FILE_OPEN_DIALOG_OPTIONS);
    if(fileToOpen.isEmpty())
        return;
    openFile(fileToOpen);
}

void BankEditor::on_actionSave_triggered()
{
    saveFileAs();
}

void BankEditor::on_actionSaveInstrument_triggered()
{
    saveInstFileAs();
}

void BankEditor::on_actionExit_triggered()
{
    this->close();
}

void BankEditor::on_actionCopy_triggered()
{
    if(!m_curInst) return;
    memcpy(&m_clipboard, m_curInst, sizeof(FmBank::Instrument));
}

void BankEditor::on_actionPaste_triggered()
{
    if(!m_curInst) return;
    memcpy(m_curInst, &m_clipboard, sizeof(FmBank::Instrument));
    flushInstrument();
    syncInstrumentName();
}

void BankEditor::on_actionReset_current_instrument_triggered()
{
    if(!m_curInstBackup || !m_curInst)
        return; //Some pointer is Null!!!
    if(memcmp(m_curInst, m_curInstBackup, sizeof(FmBank::Instrument)) == 0)
        return; //Nothing to do
    if(QMessageBox::Yes == QMessageBox::question(this,
            tr("Reset instrument to initial state"),
            tr("This instrument will be reseted to initial state "
               "(sice this file loaded or saved).\n"
               "Are you wish to continue?"),
            QMessageBox::Yes | QMessageBox::No))
    {
        memcpy(m_curInst, m_curInstBackup, sizeof(FmBank::Instrument));
        flushInstrument();
        syncInstrumentName();
    }
}

void BankEditor::on_actionReMeasure_triggered()
{
    int reply = QMessageBox::question(this,
                          tr("Are you sure?"),
                          tr("All sounding delays measures will be re-calculated. "
                             "This operation may take a while. Do you want to continue? "
                             "You may cancel operation in any moment."),
                          QMessageBox::Yes|QMessageBox::Cancel);
    if(reply == QMessageBox::Yes)
    {
        if(m_measurer->doMeasurement(m_bank, m_bankBackup, true))
            statusBar()->showMessage(tr("Sounding delays calculation has been completed!"), 5000);
        else
            statusBar()->showMessage(tr("Sounding delays calculation was canceled!"), 5000);
    }
}

void BankEditor::on_actionChipsBenchmark_triggered()
{
    if(m_curInst)
    {
        QVector<Measurer::BenchmarkResult> res;
        m_measurer->runBenchmark(*m_curInst, res);
        QString resStr;
        for(Measurer::BenchmarkResult &r : res)
            resStr += QString("%1 passed in %2 milliseconds.\n").arg(r.name).arg(r.elapsed);
        QMessageBox::information(this,
                                 tr("Benchmark result"),
                                 tr("Result of emulators benchmark based on '%1' instrument:\n\n%2")
                                 .arg(m_curInst->name)
                                 .arg(resStr)
                                 );
    }
    else
    {
        QMessageBox::information(this,
                                 tr("Instrument is not selected"),
                                 tr("Please select any instrument to begin the benchmark of emulators!"));
    }
}

void BankEditor::on_actionFormatsSup_triggered()
{
    formats_sup sup(this);
    sup.exec();
}

void BankEditor::on_actionAbout_triggered()
{
    QMessageBox::about(this,
                       tr("About bank editor"),
                       tr("FM Bank Editor for Yamaha OPN2 chip, Version %1\n\n"
                          "%2\n"
                          "\n"
                          "Licensed under GNU GPLv3\n\n"
                          "Source code available on GitHub:\n"
                          "%3")
                       .arg(VERSION)
                       .arg(COPYRIGHT)
                       .arg("https://github.com/Wohlstand/OPN2BankEditor"));
}

void BankEditor::on_instruments_currentItemChanged(QListWidgetItem *current, QListWidgetItem *)
{
    if(!current)
    {
        //ui->curInsInfo->setText("<Not Selected>");
        m_curInst = nullptr;
        m_curInstBackup = nullptr;
    }
    else
    {
        //ui->curInsInfo->setText(QString("%1 - %2").arg(current->data(Qt::UserRole).toInt()).arg(current->text()));
        setCurrentInstrument(current->data(Qt::UserRole).toInt(), ui->percussion->isChecked());
    }

    flushInstrument();
}

void BankEditor::toggleEmulator()
{
    QObject *menuItem = sender();
    ui->actionEmulatorNuked->setChecked(false);
    ui->actionEmulatorMame->setChecked(false);
    ui->actionEmulatorGens->setChecked(false);
    if(menuItem == ui->actionEmulatorNuked)
    {
        ui->actionEmulatorNuked->setChecked(true);
        m_currentChip = Generator::CHIP_Nuked;
        m_generator->switchChip(m_currentChip);
    }
    else
    if(menuItem == ui->actionEmulatorGens)
    {
        ui->actionEmulatorGens->setChecked(true);
        m_currentChip = Generator::CHIP_GENS;
        m_generator->switchChip(m_currentChip);
    }
    else
    if(menuItem == ui->actionEmulatorMame)
    {
        ui->actionEmulatorMame->setChecked(true);
        m_currentChip = Generator::CHIP_MAME;
        m_generator->switchChip(m_currentChip);
    }
}


void BankEditor::setCurrentInstrument(int num, bool isPerc)
{
    m_recentNum = num;
    m_recentPerc = isPerc;

    if(num >= 0)
    {
        m_curInst = isPerc ? &m_bank.Ins_Percussion[num] : &m_bank.Ins_Melodic[num];
        m_curInstBackup = isPerc ?
                          (num < m_bankBackup.countDrums() ? &m_bankBackup.Ins_Percussion[num] : nullptr) :
                          (num < m_bankBackup.countMelodic() ? &m_bankBackup.Ins_Melodic[num] : nullptr);
    }
    else
    {
        m_curInst = nullptr;
        m_curInstBackup = nullptr;
    }
}

void BankEditor::loadInstrument()
{
    if(!m_curInst)
    {
        ui->editzone->setEnabled(false);
        ui->editzone2->setEnabled(false);
        ui->testNoteBox->setEnabled(false);
        ui->piano->setEnabled(false);
        m_lock = true;
        ui->insName->setEnabled(false);
        ui->insName->clear();
        m_lock = false;
        return;
    }
    ui->editzone->setEnabled(true);
    ui->editzone2->setEnabled(true);
    ui->testNoteBox->setEnabled(true);
    ui->piano->setEnabled(ui->melodic->isChecked());
    ui->insName->setEnabled(true);
    m_lock = true;

    ui->lfoEnable->setChecked(m_bank.lfo_enabled);
    ui->lfoFrequency->setCurrentIndex(m_bank.lfo_frequency);

    ui->insName->setText(m_curInst->name);
    ui->perc_noteNum->setValue(m_curInst->percNoteNum);
    ui->noteOffset1->setValue(m_curInst->note_offset1);

    ui->feedback1->setValue(m_curInst->feedback);
    ui->amsens->setCurrentIndex(m_curInst->am);
    ui->fmsens->setCurrentIndex(m_curInst->fm);
    ui->algorithm->setCurrentIndex(m_curInst->algorithm);

    ui->op1_attack->setValue(       m_curInst->OP[OPERATOR1_HR].attack);
    ui->op1_decay1->setValue(       m_curInst->OP[OPERATOR1_HR].decay1);
    ui->op1_decay2->setValue(       m_curInst->OP[OPERATOR1_HR].decay2);
    ui->op1_sustain->setValue(      m_curInst->OP[OPERATOR1_HR].sustain);
    ui->op1_release->setValue(      m_curInst->OP[OPERATOR1_HR].release);
    ui->op1_am->setChecked(         m_curInst->OP[OPERATOR1_HR].am_enable);
    ui->op1_freqmult->setValue(     m_curInst->OP[OPERATOR1_HR].fmult);
    ui->op1_level->setValue(        m_curInst->OP[OPERATOR1_HR].level);
    ui->op1_detune->setCurrentIndex(m_curInst->OP[OPERATOR1_HR].detune);
    ui->op1_ratescale->setValue(    m_curInst->OP[OPERATOR1_HR].ratescale);
    ui->op1_ssgeg->setCurrentIndex( m_curInst->OP[OPERATOR1_HR].ssg_eg);

    ui->op2_attack->setValue(       m_curInst->OP[OPERATOR2_HR].attack);
    ui->op2_decay1->setValue(       m_curInst->OP[OPERATOR2_HR].decay1);
    ui->op2_decay2->setValue(       m_curInst->OP[OPERATOR2_HR].decay2);
    ui->op2_sustain->setValue(      m_curInst->OP[OPERATOR2_HR].sustain);
    ui->op2_release->setValue(      m_curInst->OP[OPERATOR2_HR].release);
    ui->op2_am->setChecked(         m_curInst->OP[OPERATOR2_HR].am_enable);
    ui->op2_freqmult->setValue(     m_curInst->OP[OPERATOR2_HR].fmult);
    ui->op2_level->setValue(        m_curInst->OP[OPERATOR2_HR].level);
    ui->op2_detune->setCurrentIndex(m_curInst->OP[OPERATOR2_HR].detune);
    ui->op2_ratescale->setValue(    m_curInst->OP[OPERATOR2_HR].ratescale);
    ui->op2_ssgeg->setCurrentIndex( m_curInst->OP[OPERATOR2_HR].ssg_eg);

    ui->op3_attack->setValue(       m_curInst->OP[OPERATOR3_HR].attack);
    ui->op3_decay1->setValue(       m_curInst->OP[OPERATOR3_HR].decay1);
    ui->op3_decay2->setValue(       m_curInst->OP[OPERATOR3_HR].decay2);
    ui->op3_sustain->setValue(      m_curInst->OP[OPERATOR3_HR].sustain);
    ui->op3_release->setValue(      m_curInst->OP[OPERATOR3_HR].release);
    ui->op3_am->setChecked(         m_curInst->OP[OPERATOR3_HR].am_enable);
    ui->op3_freqmult->setValue(     m_curInst->OP[OPERATOR3_HR].fmult);
    ui->op3_level->setValue(        m_curInst->OP[OPERATOR3_HR].level);
    ui->op3_detune->setCurrentIndex(m_curInst->OP[OPERATOR3_HR].detune);
    ui->op3_ratescale->setValue(    m_curInst->OP[OPERATOR3_HR].ratescale);
    ui->op3_ssgeg->setCurrentIndex( m_curInst->OP[OPERATOR3_HR].ssg_eg);

    ui->op4_attack->setValue(       m_curInst->OP[OPERATOR4_HR].attack);
    ui->op4_decay1->setValue(       m_curInst->OP[OPERATOR4_HR].decay1);
    ui->op4_decay2->setValue(       m_curInst->OP[OPERATOR4_HR].decay2);
    ui->op4_sustain->setValue(      m_curInst->OP[OPERATOR4_HR].sustain);
    ui->op4_release->setValue(      m_curInst->OP[OPERATOR4_HR].release);
    ui->op4_am->setChecked(         m_curInst->OP[OPERATOR4_HR].am_enable);
    ui->op4_freqmult->setValue(     m_curInst->OP[OPERATOR4_HR].fmult);
    ui->op4_level->setValue(        m_curInst->OP[OPERATOR4_HR].level);
    ui->op4_detune->setCurrentIndex(m_curInst->OP[OPERATOR4_HR].detune);
    ui->op4_ratescale->setValue(    m_curInst->OP[OPERATOR4_HR].ratescale);
    ui->op4_ssgeg->setCurrentIndex( m_curInst->OP[OPERATOR4_HR].ssg_eg);

    m_lock = false;
}

void BankEditor::sendPatch()
{
    if(!m_curInst) return;
    if(!m_generator) return;
    m_generator->changePatch(*m_curInst, ui->percussion->isChecked());
}

void BankEditor::setDrumMode(bool dmode)
{
    if(dmode)
    {
        if(ui->noteToTest->isEnabled())
            m_recentMelodicNote = ui->noteToTest->value();
    }
    else
        ui->noteToTest->setValue(m_recentMelodicNote);
    ui->noteToTest->setDisabled(dmode);
    ui->testMajor->setDisabled(dmode);
    ui->testMinor->setDisabled(dmode);
    ui->testAugmented->setDisabled(dmode);
    ui->testDiminished->setDisabled(dmode);
    ui->testMajor7->setDisabled(dmode);
    ui->testMinor7->setDisabled(dmode);
    ui->piano->setDisabled(dmode);
}

bool BankEditor::isDrumsMode()
{
    return !ui->melodic->isChecked() || ui->percussion->isChecked();
}

void BankEditor::reloadBanks()
{
    ui->bank_no->clear();
    int countOfBanks = 1;
    bool isDrum = isDrumsMode();
    if(isDrum)
        countOfBanks = ((m_bank.countDrums() - 1) / 128) + 1;
    else
        countOfBanks = ((m_bank.countMelodic() - 1) / 128) + 1;
    for(int i = 0; i < countOfBanks; i++)
    {
        const char *label = isDrum ? m_bank.Banks_Percussion[i].name : m_bank.Banks_Melodic[i].name;
        if(label[0] == 0)
            ui->bank_no->addItem(QString("Bank %1").arg(i), i);
        else
            ui->bank_no->addItem(QString("%1: %2").arg(i).arg(label), i);
    }
}

void BankEditor::on_actionAdLibBnkMode_triggered(bool checked)
{
    ui->bankListFrame->setHidden(checked);
    ui->bankRename->setHidden(checked);
    ui->bank_no->setHidden(checked);
    ui->bank_lsbmsb->setHidden(checked);
    ui->actionAddBank->setDisabled(checked);
    ui->actionCloneBank->setDisabled(checked);
    ui->actionClearBank->setDisabled(checked);
    ui->actionDeleteBank->setDisabled(checked);
    if(checked)
        on_bank_no_currentIndexChanged(ui->bank_no->currentIndex());
    else
    {
        QList<QListWidgetItem *> selected = ui->instruments->selectedItems();
        if(!selected.isEmpty())
            ui->bank_no->setCurrentIndex(selected.front()->data(INS_BANK_ID).toInt());
    }
}

void BankEditor::on_bankRename_clicked()
{
    int index = ui->bank_no->currentIndex();
    QString label;
    if(isDrumsMode())
        label = QString::fromUtf8(m_bank.Banks_Percussion[index].name);
    else
        label = QString::fromUtf8(m_bank.Banks_Melodic[index].name);
    bool ok = false;
    label = QInputDialog::getText(this, tr("Change name of bank"), tr("Please type name of current bank (32 characters max):"), QLineEdit::EchoMode::Normal, label, &ok);
    if(ok)
    {
        QByteArray arr = label.toUtf8();
        if(isDrumsMode())
        {
            memset(m_bank.Banks_Percussion[index].name, 0, 32);
            memcpy(m_bank.Banks_Percussion[index].name, arr.data(), (size_t)arr.size());
        }
        else
        {
            memset(m_bank.Banks_Melodic[index].name, 0, 32);
            memcpy(m_bank.Banks_Melodic[index].name, arr.data(), (size_t)arr.size());
        }
        if(arr.size() == 0)
            ui->bank_no->setItemText(index, QString("Bank %1").arg(index));
        else
            ui->bank_no->setItemText(index, QString("%1: %2").arg(index).arg(label));
    }
}

void BankEditor::on_bank_no_currentIndexChanged(int index)
{
    ui->bank_no->setHidden(ui->actionAdLibBnkMode->isChecked());
    ui->bank_lsbmsb->setHidden(ui->actionAdLibBnkMode->isChecked() || (index < 0));
    ui->bank_lsbmsb->setDisabled(index <= 0);
    if(index >= 0)
    {
        this->m_lock = true;
        if(isDrumsMode())
        {
            ui->bank_lsb->setValue(m_bank.Banks_Percussion[index].lsb);
            ui->bank_msb->setValue(m_bank.Banks_Percussion[index].msb);
        } else {
            ui->bank_lsb->setValue(m_bank.Banks_Melodic[index].lsb);
            ui->bank_msb->setValue(m_bank.Banks_Melodic[index].msb);
        }
        this->m_lock = false;
    }
    QList<QListWidgetItem *> items = ui->instruments->findItems("*", Qt::MatchWildcard);
    for(QListWidgetItem *it : items)
        it->setHidden(!ui->actionAdLibBnkMode->isChecked() && (it->data(INS_BANK_ID) != index));
    QList<QListWidgetItem *> selected = ui->instruments->selectedItems();
    if(!selected.isEmpty())
        ui->instruments->scrollToItem(selected.front());
}

void BankEditor::on_bank_msb_editingFinished()
{
    if(m_lock)
        return;
    int index = ui->bank_no->currentIndex();
    if(index > 0)//Allow set only non-default
    {
        if(isDrumsMode())
            m_bank.Banks_Percussion[index].msb = uint8_t(ui->bank_msb->value());
        else
            m_bank.Banks_Melodic[index].msb = uint8_t(ui->bank_msb->value());
    }
}

void BankEditor::on_bank_lsb_editingFinished()
{
    if(m_lock)
        return;
    int index = ui->bank_no->currentIndex();
    if(index > 0)//Allow set only non-default
    {
        if(isDrumsMode())
            m_bank.Banks_Percussion[index].lsb = uint8_t(ui->bank_lsb->value());
        else
            m_bank.Banks_Melodic[index].lsb = uint8_t(ui->bank_lsb->value());
    }
}


void BankEditor::setMelodic()
{
    setDrumMode(false);
    reloadBanks();
    ui->instruments->clear();
    for(int i = 0; i < m_bank.countMelodic(); i++)
    {
        QListWidgetItem *item = new QListWidgetItem();
        item->setText(m_bank.Ins_Melodic[i].name[0] != '\0' ?
                      m_bank.Ins_Melodic[i].name : getMidiInsNameM(i));
        setInstrumentMetaInfo(item, i);
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        ui->instruments->addItem(item);
    }
    on_bank_no_currentIndexChanged(ui->bank_no->currentIndex());
}

void BankEditor::setDrums()
{
    setDrumMode(true);
    reloadBanks();
    ui->instruments->clear();
    for(int i = 0; i < m_bank.countDrums(); i++)
    {
        QListWidgetItem *item = new QListWidgetItem();
        item->setText(m_bank.Ins_Percussion[i].name[0] != '\0' ?
                      m_bank.Ins_Percussion[i].name : getMidiInsNameP(i));
        setInstrumentMetaInfo(item, i);
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        ui->instruments->addItem(item);
    }
    on_bank_no_currentIndexChanged(ui->bank_no->currentIndex());
}

void BankEditor::reloadInstrumentNames()
{
    QList<QListWidgetItem *> items = ui->instruments->findItems("*", Qt::MatchWildcard);
    if(ui->percussion->isChecked())
    {
        if(items.size() != m_bank.Ins_Percussion_box.size())
            setDrums();//Completely rebuild an instruments list
        else
        {
            //Change instrument names of existing entries
            for(int i = 0; i < items.size(); i++)
            {
                int index = items[i]->data(Qt::UserRole).toInt();
                items[i]->setText(m_bank.Ins_Percussion[index].name[0] != '\0' ?
                                  m_bank.Ins_Percussion[index].name :
                                  getMidiInsNameP(index));
            }
        }
    }
    else
    {
        if(items.size() != m_bank.Ins_Melodic_box.size())
            setMelodic();//Completely rebuild an instruments list
        else
        {
            //Change instrument names of existing entries
            for(int i = 0; i < items.size(); i++)
            {
                int index = items[i]->data(Qt::UserRole).toInt();
                items[i]->setText(m_bank.Ins_Melodic[index].name[0] != '\0' ?
                                  m_bank.Ins_Melodic[index].name :
                                  getMidiInsNameM(index));
            }
        }
    }
}

void BankEditor::on_actionAddInst_triggered()
{
    FmBank::Instrument ins = FmBank::emptyInst();
    int id = 0;
    QListWidgetItem *item = new QListWidgetItem();

    if(ui->melodic->isChecked())
    {
        m_bank.Ins_Melodic_box.push_back(ins);
        m_bank.Ins_Melodic = m_bank.Ins_Melodic_box.data();
        ins = m_bank.Ins_Melodic_box.last();
        id = m_bank.countMelodic() - 1;
        item->setText(ins.name[0] != '\0' ? ins.name : getMidiInsNameM(id));
    }
    else
    {
        FmBank::Instrument ins = FmBank::emptyInst();
        m_bank.Ins_Percussion_box.push_back(ins);
        m_bank.Ins_Percussion = m_bank.Ins_Percussion_box.data();
        ins = m_bank.Ins_Percussion_box.last();
        id = m_bank.countDrums() - 1;
        item->setText(ins.name[0] != '\0' ? ins.name : getMidiInsNameP(id));
    }

    setInstrumentMetaInfo(item, id);
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    ui->instruments->addItem(item);
    int oldCount = ui->bank_no->count();
    reloadBanks();
    if(oldCount < ui->bank_no->count())
    {
        if(isDrumsMode())
            m_bank.Banks_Percussion.push_back(FmBank::emptyBank(uint16_t(m_bank.Banks_Percussion.count())));
        else
            m_bank.Banks_Melodic.push_back(FmBank::emptyBank(uint16_t(m_bank.Banks_Melodic.count())));
    }
    ui->bank_no->setCurrentIndex(ui->bank_no->count() - 1);
    ui->instruments->scrollToItem(item);
    item->setSelected(true);
    on_instruments_currentItemChanged(item, nullptr);
}

void BankEditor::on_actionClearInstrument_triggered()
{
    QList<QListWidgetItem *> selected = ui->instruments->selectedItems();
    if(!m_curInst || selected.isEmpty())
    {
        QMessageBox::warning(this,
                             tr("Instrument is not selected"),
                             tr("Select instrument to clear please"));
        return;
    }

    memset(m_curInst, 0, sizeof(FmBank::Instrument));
    loadInstrument();
    syncInstrumentName();
}

void BankEditor::on_actionDelInst_triggered()
{
    QList<QListWidgetItem *> selected = ui->instruments->selectedItems();
    if(!m_curInst || selected.isEmpty())
    {
        QMessageBox::warning(this,
                             tr("Instrument is not selected"),
                             tr("Select instrument to remove please"));
        return;
    }

    int reply = QMessageBox::question(this,
                                      tr("Single instrument deletion"),
                                      tr("Deletion of instrument will cause offset of all next instrument indexes. "
                                         "Suggested to use 'Clear instrument' action instead. "
                                         "Do you want continue deletion?"), QMessageBox::Yes | QMessageBox::No);

    if(reply == QMessageBox::Yes)
    {
        QListWidgetItem *tokill = selected.first();

        if(ui->melodic->isChecked())
        {
            m_bank.Ins_Melodic_box.remove(tokill->data(INS_INDEX).toInt());
            m_bank.Ins_Melodic = m_bank.Ins_Melodic_box.data();
        }
        else
        {
            m_bank.Ins_Percussion_box.remove(tokill->data(INS_INDEX).toInt());
            m_bank.Ins_Percussion = m_bank.Ins_Percussion_box.data();
        }

        m_curInst = nullptr;
        ui->instruments->removeItemWidget(tokill);
        selected.clear();
        delete tokill;

        // Recount indeces
        QList<QListWidgetItem *> leftItems = ui->instruments->findItems("*", Qt::MatchWildcard);
        int counter = 0;
        for(QListWidgetItem *it : leftItems)
            setInstrumentMetaInfo(it, counter++);
        reloadInstrumentNames();
        int oldBank = ui->bank_no->currentIndex();
        reloadBanks();
        if(oldBank >= ui->bank_no->count())
        {
            if(isDrumsMode())
                m_bank.Banks_Percussion.remove(oldBank);
            else
                m_bank.Banks_Melodic.remove(oldBank);
            ui->bank_no->setCurrentIndex(ui->bank_no->count() - 1);
        }
        else
            ui->bank_no->setCurrentIndex(oldBank);
        loadInstrument();
    }
}

void BankEditor::on_actionAddBank_triggered()
{
    if(ui->actionAdLibBnkMode->isChecked())
    {
        QMessageBox::information(this,
                                 tr("Add bank error"),
                                 tr("AdLib bank mode is turned on. "
                                    "Disable it to be able add or remove banks."));
        return;
    }

    if(isDrumsMode())
    {
        int oldSize = m_bank.Ins_Percussion_box.size();
        size_t addSize = 128 + ((oldSize % 128 == 0) ? 0 : (128 - (oldSize % 128)));
        size_t size = sizeof(FmBank::Instrument) * addSize;
        m_bank.Ins_Percussion_box.resize(m_bank.Ins_Percussion_box.size() + int(addSize));
        m_bank.Ins_Percussion = m_bank.Ins_Percussion_box.data();
        m_bank.Banks_Percussion.push_back(FmBank::emptyBank(uint16_t(m_bank.Banks_Percussion.count())));
        memset(m_bank.Ins_Percussion + oldSize, 0, size_t(size));
        setDrums();
    }
    else
    {
        int oldSize = m_bank.Ins_Melodic_box.size();
        size_t addSize = 128 + ((oldSize % 128 == 0) ? 0 : (128 - (oldSize % 128)));
        size_t size = sizeof(FmBank::Instrument) * addSize;
        m_bank.Ins_Melodic_box.resize(m_bank.Ins_Melodic_box.size() + int(addSize));
        m_bank.Ins_Melodic = m_bank.Ins_Melodic_box.data();
        m_bank.Banks_Melodic.push_back(FmBank::emptyBank(uint16_t(m_bank.Banks_Melodic.count())));
        memset(m_bank.Ins_Melodic + oldSize, 0, size_t(size));
        setMelodic();
    }

    reloadBanks();
    ui->bank_no->setCurrentIndex(ui->bank_no->count() - 1);
}

void BankEditor::on_actionCloneBank_triggered()
{
    if(ui->actionAdLibBnkMode->isChecked())
    {
        QMessageBox::information(this,
                                 tr("Clone bank error"),
                                 tr("AdLib bank mode is turned on. "
                                    "Disable it to be able add or remove banks."));
        return;
    }

    int curBank = ui->bank_no->currentIndex();
    int newBank = ui->bank_no->count();

    if(isDrumsMode())
    {
        int oldSize = m_bank.Ins_Percussion_box.size();
        size_t addSize = 128 + ((oldSize % 128 == 0) ? 0 : (128 - (oldSize % 128)));
        size_t size = sizeof(FmBank::Instrument) * addSize;
        m_bank.Ins_Percussion_box.resize(m_bank.Ins_Percussion_box.size() + int(addSize));
        m_bank.Ins_Percussion = m_bank.Ins_Percussion_box.data();
        memset(m_bank.Ins_Percussion + oldSize, 0, size_t(size));
        memcpy(m_bank.Ins_Percussion + (newBank * 128),
               m_bank.Ins_Percussion + (curBank * 128),
               sizeof(FmBank::Instrument) * 128);
        m_bank.Banks_Percussion.push_back(FmBank::emptyBank(uint16_t(m_bank.Banks_Percussion.count())));
        setDrums();
    }
    else
    {
        int oldSize = m_bank.Ins_Melodic_box.size();
        size_t addSize = 128 + ((oldSize % 128 == 0) ? 0 : (128 - (oldSize % 128)));
        size_t size = sizeof(FmBank::Instrument) * addSize;
        m_bank.Ins_Melodic_box.resize(m_bank.Ins_Melodic_box.size() + int(addSize));
        m_bank.Ins_Melodic = m_bank.Ins_Melodic_box.data();
        memset(m_bank.Ins_Melodic + oldSize, 0, size_t(size));
        memcpy(m_bank.Ins_Melodic + (newBank * 128),
               m_bank.Ins_Melodic + (curBank * 128),
               sizeof(FmBank::Instrument) * 128);
        m_bank.Banks_Melodic.push_back(FmBank::emptyBank(uint16_t(m_bank.Banks_Melodic.count())));
        setMelodic();
    }

    reloadBanks();
    ui->bank_no->setCurrentIndex(ui->bank_no->count() - 1);
}

void BankEditor::on_actionClearBank_triggered()
{
    if(ui->actionAdLibBnkMode->isChecked())
    {
        QMessageBox::information(this,
                                 tr("Clear bank error"),
                                 tr("AdLib bank mode is turned on. "
                                    "Disable it to be able clear banks."));
        return;
    }
    int reply = QMessageBox::question(this,
                                      tr("128-instrument bank clearing"),
                                      tr("All instruments in this bank will be cleared. "
                                         "Do you want continue deletion?"), QMessageBox::Yes | QMessageBox::No);

    if(reply == QMessageBox::Yes)
    {
        int curBank = ui->bank_no->currentIndex();
        int needToShoot_begin   = (curBank * 128);
        int needToShoot_end     = ((curBank + 1) * 128);

        if(isDrumsMode())
        {
            if(needToShoot_end >= m_bank.Ins_Percussion_box.size())
                needToShoot_end = m_bank.Ins_Percussion_box.size();
            memset(m_bank.Ins_Percussion + needToShoot_begin,
                   0,
                   sizeof(FmBank::Instrument) * size_t(needToShoot_end - needToShoot_begin));
        }
        else
        {
            if(needToShoot_end >= m_bank.Ins_Melodic_box.size())
                needToShoot_end = m_bank.Ins_Melodic_box.size();
            memset(m_bank.Ins_Melodic + needToShoot_begin,
                   0,
                   sizeof(FmBank::Instrument) * size_t(needToShoot_end - needToShoot_begin));
        }
        reloadInstrumentNames();
        loadInstrument();
    }
}

void BankEditor::on_actionDeleteBank_triggered()
{
    if(ui->actionAdLibBnkMode->isChecked())
    {
        QMessageBox::information(this,
                                 tr("Delete bank error"),
                                 tr("AdLib bank mode is turned on. "
                                    "Disable it to be able add or remove banks."));
        return;
    }

    if((ui->bank_no->currentIndex() == 0) && (ui->bank_no->count() <= 1))
    {
        QMessageBox::warning(this,
                             tr("Delete bank error"),
                             tr("Removing of last bank is not allowed!"));
        return;
    }

    int reply = QMessageBox::question(this,
                                      tr("128-instrument bank deletion"),
                                      tr("Deletion of bank will cause offset of all next bank indexes. "
                                         "Suggested to use 'Clear bank' action instead. "
                                         "Do you want continue deletion?"), QMessageBox::Yes | QMessageBox::No);

    if(reply == QMessageBox::Yes)
    {
        int curBank = ui->bank_no->currentIndex();
        int needToShoot_begin   = (curBank * 128);
        int needToShoot_end     = ((curBank + 1) * 128);

        if(isDrumsMode())
        {
            if(needToShoot_end >= m_bank.Ins_Percussion_box.size())
                needToShoot_end = m_bank.Ins_Percussion_box.size();
            m_bank.Ins_Percussion_box.remove(needToShoot_begin, needToShoot_end - needToShoot_begin);
            m_bank.Ins_Percussion = m_bank.Ins_Percussion_box.data();
            m_bank.Banks_Percussion.remove(curBank);
            setDrums();
        }
        else
        {
            if(needToShoot_end >= m_bank.Ins_Melodic_box.size())
                needToShoot_end = m_bank.Ins_Melodic_box.size();
            m_bank.Ins_Melodic_box.remove(needToShoot_begin, needToShoot_end - needToShoot_begin);
            m_bank.Ins_Melodic = m_bank.Ins_Melodic_box.data();
            m_bank.Banks_Melodic.remove(curBank);
            setMelodic();
        }

        reloadBanks();
        if(curBank >= ui->bank_no->count())
            ui->bank_no->setCurrentIndex(ui->bank_no->count() - 1);
        else
            ui->bank_no->setCurrentIndex(curBank);
    }
}

