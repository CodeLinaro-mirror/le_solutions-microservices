# CreateEvalRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**name** | **str** | The name of the evaluation. | [optional] 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 
**data_source_config** | [**CreateEvalRequestDataSourceConfig**](CreateEvalRequestDataSourceConfig.md) |  | 
**testing_criteria** | [**List[CreateEvalRequestTestingCriteriaInner]**](CreateEvalRequestTestingCriteriaInner.md) | A list of graders for all eval runs in this group. | 

## Example

```python
from openapi_client.models.create_eval_request import CreateEvalRequest

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalRequest from a JSON string
create_eval_request_instance = CreateEvalRequest.from_json(json)
# print the JSON string representation of the object
print(CreateEvalRequest.to_json())

# convert the object into a dict
create_eval_request_dict = create_eval_request_instance.to_dict()
# create an instance of CreateEvalRequest from a dict
create_eval_request_from_dict = CreateEvalRequest.from_dict(create_eval_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


