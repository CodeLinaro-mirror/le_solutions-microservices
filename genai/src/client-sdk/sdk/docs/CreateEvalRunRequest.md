# CreateEvalRunRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**name** | **str** | The name of the run. | [optional] 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 
**data_source** | [**CreateEvalRunRequestDataSource**](CreateEvalRunRequestDataSource.md) |  | 

## Example

```python
from openapi_client.models.create_eval_run_request import CreateEvalRunRequest

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalRunRequest from a JSON string
create_eval_run_request_instance = CreateEvalRunRequest.from_json(json)
# print the JSON string representation of the object
print(CreateEvalRunRequest.to_json())

# convert the object into a dict
create_eval_run_request_dict = create_eval_run_request_instance.to_dict()
# create an instance of CreateEvalRunRequest from a dict
create_eval_run_request_from_dict = CreateEvalRunRequest.from_dict(create_eval_run_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


