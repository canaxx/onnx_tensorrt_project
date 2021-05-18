#include "assert.h"
#include <cuda_runtime.h>
#include <opencv2/opencv.hpp>
#include <common.h>
#include "Trt.h"

typedef std::vector<cv::Mat> BatchResult;

class Unet
{
public:
	Config _config;
	uint32_t m_InputH;
	uint32_t m_InputW;
	uint32_t m_InputC;
	uint32_t m_InputSize;
	uint32_t m_BatchSize = 1;
	cudaStream_t mCudaStream;
	std::shared_ptr<Trt> onnx_net;
	std::vector<std::string> m_ClassNames;
	std::vector<TensorInfo> m_OutputTensors;
	float CONF_THRESH = 0.6;
	Unet::Unet()
	{

	}
	Unet::~Unet()
	{
		//�ͷ��ڴ�
		m_ClassNames.clear();
		for (int i = 0; i < m_OutputTensors.size(); i++)
		{
			m_OutputTensors[i].hostBuffer.clear();
		}
		m_OutputTensors.clear();
		cudaStreamDestroy(mCudaStream);
	}

	void UpdateOutputTensor()
	{
		m_InputW = onnx_net->mBindingDims[0].d[2];
		m_InputH = onnx_net->mBindingDims[0].d[3];
		for (int m_yolo_ind = 1; m_yolo_ind < onnx_net->mBindingName.size(); m_yolo_ind++)
		{
			TensorInfo outputTensor;
			outputTensor.volume = onnx_net->mBindingSize[m_yolo_ind] / sizeof(float);
			outputTensor.blobName = onnx_net->mBindingName[m_yolo_ind];
			m_OutputTensors.push_back(outputTensor);
		}
	}

	void allocateBuffers()
	{
		for (auto& tensor : m_OutputTensors)
		{
			tensor.bindingIndex = onnx_net->mEngine->getBindingIndex(tensor.blobName.c_str());
			assert((tensor.bindingIndex != -1) && "Invalid output binding index");
			tensor.hostBuffer.resize(tensor.volume * m_BatchSize /** sizeof(float)*/);
		}
	}

	void doInference(std::vector<float> input, const uint32_t batchSize)
	{
		//	Timer timer;
		assert(batchSize <= m_BatchSize && "Image batch size exceeds TRT engines batch size");
		onnx_net->CopyFromHostToDevice(input, 0, mCudaStream);
		onnx_net->ForwardAsync(mCudaStream);
		for (auto& tensor : m_OutputTensors)
		{
			onnx_net->CopyFromDeviceToHost(tensor.hostBuffer, tensor.bindingIndex, mCudaStream);
		}
		cudaStreamSynchronize(mCudaStream);
	}

	std::vector<float> postprocess(std::vector<float> buffer)
	{
		// Softmax function
		std::transform(buffer.begin(), buffer.end(), buffer.begin(), [](float val) {return std::exp(val); });
		float sum = std::accumulate(buffer.begin(), buffer.end(), 0.0);
		if (sum > 0)
		{
			for (int i = 0; i < buffer.size(); i++)
			{
				buffer[i] /= sum;
			}
		}
		return buffer;
	}

	struct  Detection {
		std::vector<float> mask;
	};

	float sigmoid(float x)
	{
		return (1 / (1 + exp(-x)));
	}

	void process_cls_result(Detection& res, float* output) {
		res.mask.resize(m_InputW * m_InputH * 1);
		for (int i = 0; i < m_InputW * m_InputH * 1; i++) {
			res.mask[i] = sigmoid(*(output + i));
		}
	}


	void isInt8(std::string calibration_image_list_file, int width, int height)
	{
		size_t npos = _config.onnxModelpath.find_first_of('.');
		std::string calib_table_name = _config.onnxModelpath.substr(0, npos) + ".table";
		if (!fileExists(calib_table_name))
		{
			onnx_net->SetInt8Calibrator("Int8MinMaxCalibrator", width, height, calibration_image_list_file.c_str(), calib_table_name.c_str());
		}
	}

	void init(Config config)
	{
		_config = config;
		onnx_net = std::make_shared<Trt>();
		onnx_net->SetMaxBatchSize(config.maxBatchSize);
		if (config.mode == 2)
		{
			isInt8(config.calibration_image_list_file, config.calibration_width, config.calibration_height);
		}
		m_BatchSize = config.maxBatchSize;
		onnx_net->CreateEngine(config.onnxModelpath, config.engineFile, config.customOutput, config.maxBatchSize, config.mode);
		//����m_OutputTensors
		UpdateOutputTensor();
		allocateBuffers();
		cudaStreamCreate(&mCudaStream);
	}

	void detect(const std::vector<cv::Mat>& vec_image,
		std::vector<BatchResult>& vec_batch_result)
	{
		vec_batch_result.clear();
		vec_batch_result.resize(vec_image.size());
		std::vector<float> data;
		for (const auto& img : vec_image)
		{
			cv::Mat resized, imgf;
			cv::resize(img, resized, cv::Size(m_InputH, m_InputW));
			resized.convertTo(imgf, CV_32FC3, 1 / 255.0);
			std::vector<cv::Mat>channles(3);
			cv::split(imgf, channles);
			float* ptr1 = (float*)(channles[0].data);
			float* ptr2 = (float*)(channles[1].data);
			float* ptr3 = (float*)(channles[2].data);
			data.insert(data.end(), ptr1, ptr1 + m_InputH * m_InputW);
			data.insert(data.end(), ptr2, ptr2 + m_InputH * m_InputW);
			data.insert(data.end(), ptr3, ptr3 + m_InputH * m_InputW);
		}
		doInference(data, vec_image.size());
		for (int i = 0; i < vec_image.size(); i++)
		{
			float max_conf = 0.0;
			int max_indice;
			Detection m_Detection;
			for (auto& tensor : m_OutputTensors)
			{
				process_cls_result(m_Detection, tensor.hostBuffer.data());
			}
			float* mask = m_Detection.mask.data();
			cv::Mat mask_mat = cv::Mat(m_InputH, m_InputW, CV_8UC1);
			uchar* ptmp = NULL;
			for (int i = 0; i < m_InputH; i++) {
				ptmp = mask_mat.ptr<uchar>(i);
				for (int j = 0; j < m_InputW; j++) {
					float* pixcel = mask + i * m_InputW + j;
					if (*pixcel > CONF_THRESH) {
						ptmp[j] = 255;
					}
					else {
						ptmp[j] = 0;
					}
				}
			}
			cv::Mat mask_mat_1 = cv::Mat(m_InputH, m_InputW, CV_8UC1);
			mask_mat_1 = mask_mat.clone();
			vec_batch_result[i].push_back(mask_mat_1);
		}
	}
};

int main_unet()
{
	Unet m_Unet;
	Config m_config;
	m_config.onnxModelpath = "D:\\onnx_tensorrt\\onnx_tensorrt_centernet\\onnx_tensorrt_project\\model\\pryorch_onnx_tensorrt_unet\\unet.onnx";
	m_config.engineFile = "D:\\onnx_tensorrt\\onnx_tensorrt_centernet\\onnx_tensorrt_project\\model\\pryorch_onnx_tensorrt_unet\\unet_fp32_batch_1.engine";
	//m_config.calibration_image_list_file = "D:\\onnx_tensorrt\\onnx_tensorrt_centernet\\onnx_tensorrt_project\\model\\pryorch_onnx_tensorrt_unet\\images\\";
	//m_config.calibration_width = 224;
	//m_config.calibration_height = 224;
	m_config.maxBatchSize = 1;
	m_config.mode = 0;
	m_Unet.init(m_config);
	std::vector<BatchResult> batch_res;
	std::vector<cv::Mat> batch_img;
	std::string filename = "F:\\unet\\TCGA_CS_4944.png";
	cv::Mat image = cv::imread(filename);
	batch_img.push_back(image);
	m_Unet.detect(batch_img, batch_res);
	int a = 0;
	return 0;
}