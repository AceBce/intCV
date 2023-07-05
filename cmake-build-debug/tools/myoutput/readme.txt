addIsRelated.py用于把数据标上标签，它需要一个叫RealCorrelation.txt的文件，这个文件的每一行是一组关联的变量名，最好要文件名:::变量名，利用这个文件把llvm端输出的output.json文件标成modified_data.json文件

skXG.py用于训练模型，先读取modified_data.json文件内容，再训练把2/3的数据用于测试，1/3的数据用于测试，打印测试的结果，最后保存模型

PruePredict.py用于加载模型进行预测，加载的是名叫xgboost_model.pkl的模型



