//
// Created by mctrivia on 21/12/24.
//

#ifndef PLOTMANAGER_H
#define PLOTMANAGER_H

#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>

using namespace std;

class PlotManager : public QWidget
{
    Q_OBJECT

public:
    explicit PlotManager(QWidget *parent = nullptr);

    // Add a graph by providing data, axis labels, and a chart title.
    void addGraph(const QVector<double> &xData,
                  const QVector<double> &yData,
                  const QString &xLabel,
                  const QString &yLabel,
                  const QString &title);

private:
    QScrollArea *_scrollArea;   // Scroll area to contain multiple charts
    QWidget *_scrollWidget;     // Widget inside the scroll area
    QVBoxLayout *_vLayout;      // Vertical layout for charts
};

#endif // PLOTMANAGER_H
