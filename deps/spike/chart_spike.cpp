// chart_spike.cpp — isolated proof that vtkChartXY paints inside a
// QVTKOpenGLNativeWidget in THIS VTK 9.6 + Qt 6.11 build (the 2D context-GL
// backend the profile note flagged as suspect). Standalone exe, NOT part of the
// gmtvtk TU. If a sine curve + axes + legend show, vtkChartXY is the green light
// for the new X,Y tool.
#include <QApplication>
#include <QVTKOpenGLNativeWidget.h>
#include <QSurfaceFormat>

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkContextView.h>
#include <vtkContextScene.h>
#include <vtkChartXY.h>
#include <vtkPlot.h>
#include <vtkAxis.h>
#include <vtkTable.h>
#include <vtkFloatArray.h>
#include <vtkNew.h>

#include <cmath>

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	// MUST set the default format from QVTK before the widget is created.
	QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());

	QVTKOpenGLNativeWidget widget;
	vtkNew<vtkGenericOpenGLRenderWindow> renderWindow;
	widget.setRenderWindow(renderWindow.Get());

	// Canonical embed recipe: a vtkContextView driving the widget's render window
	// + interactor; the chart is an item in the context scene.
	vtkNew<vtkContextView> view;
	view->SetRenderWindow(renderWindow.Get());
	view->SetInteractor(widget.interactor());

	vtkNew<vtkChartXY> chart;
	view->GetScene()->AddItem(chart.Get());
	chart->SetShowLegend(true);

	// Two sample series so we exercise legend + multi-plot.
	vtkNew<vtkTable> table;
	vtkNew<vtkFloatArray> arrX;  arrX->SetName("X (s)");        table->AddColumn(arrX.Get());
	vtkNew<vtkFloatArray> arrS;  arrS->SetName("sin");          table->AddColumn(arrS.Get());
	vtkNew<vtkFloatArray> arrC;  arrC->SetName("cos*decay");    table->AddColumn(arrC.Get());

	const int n = 200;
	table->SetNumberOfRows(n);
	for (int i = 0; i < n; ++i) {
		const double t = i * 0.1;
		table->SetValue(i, 0, t);
		table->SetValue(i, 1, std::sin(t));
		table->SetValue(i, 2, std::cos(t) * std::exp(-t * 0.05));
	}

	vtkPlot* l0 = chart->AddPlot(vtkChart::LINE);
	l0->SetInputData(table.Get(), 0, 1);
	l0->SetColor(235, 170, 0, 255);
	l0->SetWidth(2.0f);

	vtkPlot* l1 = chart->AddPlot(vtkChart::LINE);
	l1->SetInputData(table.Get(), 0, 2);
	l1->SetColor(40, 120, 230, 255);
	l1->SetWidth(2.0f);

	chart->GetAxis(vtkAxis::BOTTOM)->SetTitle("X (s)");
	chart->GetAxis(vtkAxis::LEFT)->SetTitle("amplitude");

	widget.resize(720, 520);
	widget.setWindowTitle("vtkChartXY spike — InteractiveGMT");
	widget.show();

	return app.exec();
}
