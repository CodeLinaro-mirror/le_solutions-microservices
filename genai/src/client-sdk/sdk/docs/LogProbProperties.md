# LogProbProperties

A log probability object. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**token** | **str** | The token that was used to generate the log probability.  | 
**logprob** | **float** | The log probability of the token.  | 
**bytes** | **List[int]** | The bytes that were used to generate the log probability.  | 

## Example

```python
from openapi_client.models.log_prob_properties import LogProbProperties

# TODO update the JSON string below
json = "{}"
# create an instance of LogProbProperties from a JSON string
log_prob_properties_instance = LogProbProperties.from_json(json)
# print the JSON string representation of the object
print(LogProbProperties.to_json())

# convert the object into a dict
log_prob_properties_dict = log_prob_properties_instance.to_dict()
# create an instance of LogProbProperties from a dict
log_prob_properties_from_dict = LogProbProperties.from_dict(log_prob_properties_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


