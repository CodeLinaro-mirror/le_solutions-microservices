# CreateFineTuningJobRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**model** | [**CreateFineTuningJobRequestModel**](CreateFineTuningJobRequestModel.md) |  | 
**training_file** | **str** | The ID of an uploaded file that contains training data.  See [upload file](/docs/api-reference/files/create) for how to upload a file.  Your dataset must be formatted as a JSONL file. Additionally, you must upload your file with the purpose &#x60;fine-tune&#x60;.  The contents of the file should differ depending on if the model uses the [chat](/docs/api-reference/fine-tuning/chat-input), [completions](/docs/api-reference/fine-tuning/completions-input) format, or if the fine-tuning method uses the [preference](/docs/api-reference/fine-tuning/preference-input) format.  See the [fine-tuning guide](/docs/guides/fine-tuning) for more details.  | 
**hyperparameters** | [**CreateFineTuningJobRequestHyperparameters**](CreateFineTuningJobRequestHyperparameters.md) |  | [optional] 
**suffix** | **str** | A string of up to 64 characters that will be added to your fine-tuned model name.  For example, a &#x60;suffix&#x60; of \&quot;custom-model-name\&quot; would produce a model  | [optional] 
**validation_file** | **str** | The ID of an uploaded file that contains validation data.  If you provide this file, the data is used to generate validation metrics periodically during fine-tuning. These metrics can be viewed in the fine-tuning results file. The same data should not be present in both train and validation files.  Your dataset must be formatted as a JSONL file. You must upload your file with the purpose &#x60;fine-tune&#x60;.  See the [fine-tuning guide](/docs/guides/fine-tuning) for more details.  | [optional] 
**integrations** | [**List[CreateFineTuningJobRequestIntegrationsInner]**](CreateFineTuningJobRequestIntegrationsInner.md) | A list of integrations to enable for your fine-tuning job. | [optional] 
**seed** | **int** | The seed controls the reproducibility of the job. Passing in the same seed and job parameters should produce the same results, but may differ in rare cases. If a seed is not specified, one will be generated for you.  | [optional] 
**method** | [**FineTuneMethod**](FineTuneMethod.md) |  | [optional] 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 

## Example

```python
from openapi_client.models.create_fine_tuning_job_request import CreateFineTuningJobRequest

# TODO update the JSON string below
json = "{}"
# create an instance of CreateFineTuningJobRequest from a JSON string
create_fine_tuning_job_request_instance = CreateFineTuningJobRequest.from_json(json)
# print the JSON string representation of the object
print(CreateFineTuningJobRequest.to_json())

# convert the object into a dict
create_fine_tuning_job_request_dict = create_fine_tuning_job_request_instance.to_dict()
# create an instance of CreateFineTuningJobRequest from a dict
create_fine_tuning_job_request_from_dict = CreateFineTuningJobRequest.from_dict(create_fine_tuning_job_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


